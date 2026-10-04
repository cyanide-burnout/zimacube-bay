// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZimaCube drive-bay backplane controller - hwmon driver.
 *
 * The backplane controller sits on the host SMBus. This driver owns the bus
 * access and publishes the controller's readings through hwmon, so that
 * userspace can read fan speeds and the board temperature with `sensors` or
 * anything else that speaks hwmon. By default it establishes an 80% fallback
 * duty; userspace then takes control through pwm1.
 *
 * Deliberate split of responsibilities, mirroring zimacube-ec:
 *
 *   this driver   transport only - expose readings, apply a default duty
 *                 and accept manual fan commands
 *   userspace     the policy - which duty to ask for, based on drive
 *                 temperatures and spin state, which this driver does not read
 *
 * Fan reads, writes and watchdog fallback were checked on a ZimaCube Pro.
 * Explicit slot power commands are opt-in and have not been hardware-tested.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pm.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define DRVNAME "zimacube_bay"

/* Readings are cached for this long so that a `sensors` run, which reads every
 * attribute in a row, costs one bus transaction per quantity instead of one
 * per attribute. */
#define CACHE_TTL (HZ / 2)

/* This controller reports up to two bay fans. */
#define BAY_MAX_FANS 2

/* hwmon pwm is 0..255; the controller is commanded in percent. */
#define HWMON_PWM_MAX 255
#define BAY_PERCENT_MAX 100
#define BAY_WATCHDOG_MAX_SECS 3600
#define BAY_READ_SIZE 8
#define BAY_INQUIRY_SIZE 16
#define BAY_POWER_SIZE 16
#define BAY_POWER_SLOT_COUNT 7
#define BAY_VALID_OFFSET 6
#define BAY_INQUIRY_VALID_OFFSET 14
#define BAY_IDENTIFY_ATTEMPTS 3
#define BAY_IDENTIFY_RETRY_MS 75
#define BAY_FAN_RPM_SETTLE_MS 150

#define BAY_CMD_DISKS 0x01
#define BAY_CMD_FAN_RPM 0x02
#define BAY_CMD_BOARD_TEMP 0x03
#define BAY_CMD_SET_FAN 0x04
#define BAY_CMD_SET_POWER 0x05
#define BAY_CMD_INQUIRY 0x00
#define BAY_I2C_ADDRESS 0x69

/* Fan count is configured for the supported machine model. */
static unsigned int fan_count = BAY_MAX_FANS;
module_param(fan_count, uint, 0444);
MODULE_PARM_DESC(fan_count, "Number of bay fans to expose (1 or 2, default 2)");

/* This pair was observed on one live ZimaCube. Other revisions may differ. */
static int expected_vendor_id = 0x8086;
module_param(expected_vendor_id, int, 0444);
MODULE_PARM_DESC(expected_vendor_id, "Expected INQUIRY vendor ID (default 0x8086; -1: not pinned)");

static int expected_device_id = 0x1001;
module_param(expected_device_id, int, 0444);
MODULE_PARM_DESC(expected_device_id, "Expected INQUIRY device ID (default 0x1001; -1: not pinned)");

/* Fan control is on by default. Set this false for a read-only bring-up. */
static bool enable_fan_control = true;
module_param(enable_fan_control, bool, 0444);
MODULE_PARM_DESC(enable_fan_control,
                 "Expose pwm1 and permit fan writes (default true; false: no fan writes)");

/* The physical slot mapping and electrical effect have not been verified. */
static bool enable_disk_power;
module_param(enable_disk_power, bool, 0444);
MODULE_PARM_DESC(enable_disk_power,
                 "Allow explicit slot power commands (default false; untested on hardware)");

/*
 * When fan control is enabled, probe first applies safe_percent. The watchdog
 * returns to that duty if userspace stops writing after a manual pwm1 command.
 * A pwm1_enable=1 keepalive counts only after an actual pwm1 command.
 *
 * Zero disables the watchdog entirely, which is the right setting only if the
 * controller has a fail-safe of its own and you have confirmed it.
 */
static unsigned int watchdog_secs = 60;
module_param(watchdog_secs, uint, 0444);
MODULE_PARM_DESC(watchdog_secs,
                 "Seconds without a pwm1 write before falling back to safe_percent (0 disables)");

static unsigned int safe_percent = 80;
module_param(safe_percent, uint, 0444);
MODULE_PARM_DESC(safe_percent,
                 "Duty percent applied at probe and on fallback (default 80)");

/* This is a software floor, not proof that a fan starts at this duty. */
static unsigned int minimum_percent = 30;
module_param(minimum_percent, uint, 0444);
MODULE_PARM_DESC(minimum_percent,
                 "Lowest permitted fan duty percent when control is enabled (default 30)");

struct bay_data
{
  struct i2c_client *client;
  struct device *hwmon;
  struct mutex lock;

  /* cached readings */
  unsigned long fan_updated, temp_updated, disk_updated;
  bool fan_valid, temp_valid, disk_valid;
  u16 rpm[BAY_MAX_FANS];
  long board_temp_mdeg;    /* millidegrees C, as hwmon wants it */
  u8 hdd_slots;            /* raw count capped at six HDD positions */
  u8 positions_reported;   /* raw response byte 0; seven on tested Cube */
  u8 status_byte2_raw;     /* unassigned response byte 2 */
  u32 hdd_present;         /* bitmask, bit N set when bay N is occupied */
  bool nvme_present;

  /* commanded state */
  u8 commanded_percent;
  bool commanded_valid;
  unsigned long last_command;  /* jiffies stamp of the last pwm1 write */
  bool failsafe_active;
  bool hwmon_registered;

  /* identity, configured after bay_identify() at probe time */
  unsigned int fan_count;

  struct delayed_work watchdog_work;
};

/* --------------------------------------------------------------- protocol */

/* I2C block data has no count byte on the wire. A read consists of the
 * command byte followed by exactly len data bytes. The last-but-one byte in
 * each response is a validity marker. */
static int bay_read_block(struct i2c_client *client, u8 command, u8 *data,
                          u8 len, u8 valid_offset)
{
  int ret;

  ret = i2c_smbus_read_i2c_block_data(client, command, len, data);
  if (ret < 0)
    return ret;
  if (ret != len)
    return -EIO;
  if (data[valid_offset] != 1)
    return -EBADMSG;
  return 0;
}

static int bay_read_fan_rpm(struct i2c_client *client, u16 *rpm,
                            unsigned int count)
{
  u8 data[BAY_READ_SIZE];
  int ret;

  if ((count < 1) || (count > BAY_MAX_FANS))
    return -EINVAL;
  ret = bay_read_block(client, BAY_CMD_FAN_RPM, data, sizeof(data),
                       BAY_VALID_OFFSET);
  if (ret)
    return ret;
  rpm[0] = data[0] | ((u16)data[1] << 8);
  if (count == 2)
    rpm[1] = data[2] | ((u16)data[3] << 8);
  return 0;
}

static int bay_read_board_temp(struct i2c_client *client, long *mdeg)
{
  u8 data[BAY_READ_SIZE];
  u16 raw;
  int ret;

  ret = bay_read_block(client, BAY_CMD_BOARD_TEMP, data, sizeof(data),
                       BAY_VALID_OFFSET);
  if (ret)
    return ret;
  raw = data[0] | ((u16)data[1] << 8);
  /* The raw word is centidegrees Celsius; hwmon expects millidegrees. */
  *mdeg = (long)raw * 10;
  return 0;
}

static int bay_read_presence(struct i2c_client *client, u8 *positions,
                             u8 *byte2, u32 *hdd_mask, bool *nvme)
{
  u8 data[BAY_READ_SIZE];
  int ret;

  ret = bay_read_block(client, BAY_CMD_DISKS, data, sizeof(data),
                       BAY_VALID_OFFSET);
  if (ret)
    return ret;
  *positions = data[0];
  *byte2 = data[2];
  *hdd_mask = data[1] & 0x3f;
  *nvme = !!(data[1] & 0x40);
  return 0;
}

static int bay_write_fan(struct i2c_client *client, u8 percent)
{
  u8 data[BAY_READ_SIZE] = { 0 };

  if ((percent < minimum_percent) || (percent > BAY_PERCENT_MAX))
    return -ERANGE;
  data[0] = 1;
  data[1] = percent;
  data[6] = 1;
  return i2c_smbus_write_i2c_block_data(client, BAY_CMD_SET_FAN,
                                         sizeof(data), data);
}

static int bay_write_slot_power(struct i2c_client *client, unsigned int slot,
                                bool on)
{
  u8 data[BAY_POWER_SIZE] = { 0 };

  if (slot >= BAY_POWER_SLOT_COUNT)
    return -EINVAL;
  data[2 * slot] = 1;
  data[2 * slot + 1] = on ? 1 : 0;
  data[14] = 1;
  return i2c_smbus_write_i2c_block_data(client, BAY_CMD_SET_POWER,
                                         sizeof(data), data);
}

/* INQUIRY has IDs and version triplets but no fan-count field. Verify all
 * four known response types before registering hwmon. */
static int bay_identify_once(struct i2c_client *client, bool *id_mismatch)
{
  u8 data[BAY_INQUIRY_SIZE];
  u8 positions, byte2;
  u32 hdd_mask;
  bool nvme;
  u16 rpm[BAY_MAX_FANS];
  u16 vendor_id, device_id;
  long mdeg;
  int ret;

  *id_mismatch = false;
  ret = bay_read_block(client, BAY_CMD_INQUIRY, data, sizeof(data),
                       BAY_INQUIRY_VALID_OFFSET);
  if (ret)
    return ret;

  vendor_id = data[0] | ((u16)data[1] << 8);
  device_id = data[2] | ((u16)data[3] << 8);
  if (((expected_vendor_id >= 0) && (vendor_id != expected_vendor_id)) ||
      ((expected_device_id >= 0) && (device_id != expected_device_id)))
  {
    *id_mismatch = true;
    return -ENODEV;
  }

  ret = bay_read_fan_rpm(client, rpm, fan_count);
  if (ret)
    return ret;
  ret = bay_read_board_temp(client, &mdeg);
  if (ret)
    return ret;
  ret = bay_read_presence(client, &positions, &byte2, &hdd_mask, &nvme);
  if (ret)
    return ret;

  dev_info(&client->dev, "INQUIRY %04x:%04x hardware %u.%u.%u firmware %u.%u.%u\n",
           vendor_id, device_id, data[4], data[5], data[6],
           data[7], data[8], data[9]);
  return 0;
}

static int bay_identify(struct i2c_client *client)
{
  bool id_mismatch;
  bool all_id_mismatch = true;
  int attempt, ret, last_read_error = -EIO;

  /* Even a response with a valid marker can contain a transiently corrupt ID.
   * Require one complete, matching read sequence before issuing any writes. */
  for (attempt = 0; attempt < BAY_IDENTIFY_ATTEMPTS; attempt++)
  {
    ret = bay_identify_once(client, &id_mismatch);
    if (!ret)
      return 0;
    if (!id_mismatch)
    {
      all_id_mismatch = false;
      last_read_error = ret;
    }
    if ((attempt + 1) < BAY_IDENTIFY_ATTEMPTS)
      msleep(BAY_IDENTIFY_RETRY_MS);
  }
  return all_id_mismatch ? -ENODEV : last_read_error;
}

/* --------------------------------------------------------------------- cache */

/* Caller holds state->lock. A failed disk read must not hide working RPM or
 * temperature telemetry, so each command has its own cache and error path. */
static int bay_refresh_fan_locked(struct bay_data *state)
{
  u16 rpm[BAY_MAX_FANS];
  int result;

  if ((state->fan_valid) &&
      time_before(jiffies, state->fan_updated + CACHE_TTL))
    return 0;
  result = bay_read_fan_rpm(state->client, rpm, state->fan_count);
  if (result)
    return result;
  /* A zero tachometer sample can be transient. Read again after 150 ms;
   * preserve zero if the second sample is also zero. */
  if ((!rpm[0]) || ((state->fan_count == 2) && (!rpm[1])))
  {
    msleep(BAY_FAN_RPM_SETTLE_MS);
    /* Decoding writes rpm only after a complete, valid response. If the
     * second transaction fails, keep the first sample (including its zero). */
    (void)bay_read_fan_rpm(state->client, rpm, state->fan_count);
  }
  state->rpm[0] = rpm[0];
  if (state->fan_count == 2)
    state->rpm[1] = rpm[1];
  state->fan_updated = jiffies;
  state->fan_valid = true;
  return 0;
}

static int bay_refresh_temp_locked(struct bay_data *state)
{
  long temp;
  int result;

  if ((state->temp_valid) &&
      time_before(jiffies, state->temp_updated + CACHE_TTL))
    return 0;
  result = bay_read_board_temp(state->client, &temp);
  if (result)
    return result;
  state->board_temp_mdeg = temp;
  state->temp_updated = jiffies;
  state->temp_valid = true;
  return 0;
}

static int bay_refresh_disk_locked(struct bay_data *state)
{
  u8 positions, byte2;
  u32 hdd_present;
  bool nvme_present;
  int result;

  if ((state->disk_valid) &&
      time_before(jiffies, state->disk_updated + CACHE_TTL))
    return 0;
  result = bay_read_presence(state->client, &positions, &byte2, &hdd_present,
                             &nvme_present);
  if (result)
    return result;
  state->positions_reported = positions;
  state->hdd_slots = min_t(u8, positions, 6);
  state->status_byte2_raw = byte2;
  state->hdd_present = hdd_present;
  state->nvme_present = nvme_present;
  state->disk_updated = jiffies;
  state->disk_valid = true;
  return 0;
}

/* ------------------------------------------------------------------ watchdog */

/*
 * Nothing here talks to the controller unless the deadline has actually
 * passed, so the timer is cheap even at a short period.
 */
static void bay_watchdog(struct work_struct *work)
{
  struct bay_data *state = container_of(to_delayed_work(work), struct bay_data,
                                        watchdog_work);
  unsigned int secs = watchdog_secs;
  unsigned int safe = safe_percent;
  bool expired;

  if (!secs)
    goto again;

  mutex_lock(&state->lock);
  expired = (!state->failsafe_active) &&
            time_after_eq(jiffies, state->last_command + secs * HZ);
  if (expired)
  {
    int result = bay_write_fan(state->client, safe);

    if (result)
      dev_err(&state->client->dev,
              "watchdog expired but the fallback to %u%% failed: %d\n", safe,
              result);
    else
      dev_warn(&state->client->dev,
               "no pwm1 write for %us, falling back to %u%%\n", secs, safe);
    /* A failed fallback leaves the flag clear, so the next tick retries. */
    if (!result)
    {
      state->commanded_percent = safe;
      state->commanded_valid = false;
      state->failsafe_active = true;
    }
  }
  mutex_unlock(&state->lock);

again:
  /* Check four times per deadline so the fallback is not a full period late. */
  schedule_delayed_work(&state->watchdog_work,
                        secs ? max_t(long, HZ, (long)secs * HZ / 4) : 10 * HZ);
}

/* --------------------------------------------------------------------- hwmon */

static umode_t bay_is_visible(const void *drvdata, enum hwmon_sensor_types type,
                              u32 attr, int channel)
{
  const struct bay_data *state = drvdata;

  switch (type)
  {
    case hwmon_fan:
      if (channel >= (int)state->fan_count)
        return 0;
      return 0444;

    case hwmon_temp:
      return 0444;

    case hwmon_pwm:
      if (!enable_fan_control)
        return 0;
      switch (attr)
      {
        case hwmon_pwm_input:
        case hwmon_pwm_enable:
          return 0644;
        default:
          return 0;
      }

    default:
      return 0;
  }
}

static int bay_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
                    int channel, long *val)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result = 0;

  mutex_lock(&state->lock);

  switch (type)
  {
    case hwmon_fan:
      if (channel >= (int)state->fan_count)
      {
        result = -EOPNOTSUPP;
        break;
      }
      result = bay_refresh_fan_locked(state);
      if (result)
        break;
      *val = state->rpm[channel];
      break;

    case hwmon_temp:
      result = bay_refresh_temp_locked(state);
      if (result)
        break;
      *val = state->board_temp_mdeg;
      break;

    case hwmon_pwm:
      switch (attr)
      {
        case hwmon_pwm_input:
          if ((!state->commanded_valid) && (!state->failsafe_active))
          {
            result = -ENODATA;
            break;
          }
          *val = DIV_ROUND_CLOSEST(state->commanded_percent * HWMON_PWM_MAX,
                                   BAY_PERCENT_MAX);
          break;

        case hwmon_pwm_enable:
          /* 1 = a duty this driver was told to hold, 0 = nobody is steering */
          *val = state->commanded_valid ? 1 : 0;
          break;

        default:
          result = -EOPNOTSUPP;
      }
      break;

    default:
      result = -EOPNOTSUPP;
  }

  mutex_unlock(&state->lock);
  return result;
}

static int bay_read_string(struct device *dev, enum hwmon_sensor_types type,
                           u32 attr, int channel, const char **str)
{
  static const char *const fan_labels[] = { "Bay Fan 1", "Bay Fan 2" };

  if ((type == hwmon_fan) && (attr == hwmon_fan_label) &&
      (channel >= 0) && (channel < (int)ARRAY_SIZE(fan_labels)))
  {
    *str = fan_labels[channel];
    return 0;
  }

  if ((type == hwmon_temp) && (attr == hwmon_temp_label) && (channel == 0))
  {
    *str = "Backplane";
    return 0;
  }

  return -EOPNOTSUPP;
}

static int bay_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
                     int channel, long val)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result;
  u8 percent;

  (void)channel;

  if ((!enable_fan_control) || (type != hwmon_pwm))
    return -EOPNOTSUPP;

  mutex_lock(&state->lock);

  switch (attr)
  {
    case hwmon_pwm_input:
      if ((val < 0) || (val > HWMON_PWM_MAX))
      {
        result = -EINVAL;
        break;
      }
      percent = DIV_ROUND_CLOSEST(val * BAY_PERCENT_MAX, HWMON_PWM_MAX);
      result = bay_write_fan(state->client, percent);
      if (!result)
      {
        state->commanded_percent = percent;
        state->commanded_valid = true;
        state->last_command = jiffies;
        state->failsafe_active = false;
      }
      break;

    case hwmon_pwm_enable:
      /*
       * 0 means "nobody is steering", which for this controller is the same
       * thing the watchdog does: hand it the safe duty. 1 re-arms the
       * watchdog without changing the duty. There is no mode 2 here - the
       * controller has no curve of its own that this driver knows how to
       * configure, and pretending otherwise would promise something the
       * hardware does not do.
       */
      if (val == 0)
      {
        unsigned int safe = safe_percent;

        result = bay_write_fan(state->client, safe);
        if (!result)
        {
          state->commanded_percent = safe;
          state->commanded_valid = false;
          state->failsafe_active = true;
        }
      }
      else if (val == 1)
      {
        if ((!state->commanded_valid) || (state->failsafe_active))
        {
          result = -EINVAL;
        }
        else
        {
          state->last_command = jiffies;
          result = 0;
        }
      }
      else
      {
        result = -EINVAL;
      }
      break;

    default:
      result = -EOPNOTSUPP;
  }

  mutex_unlock(&state->lock);
  return result;
}

static const struct hwmon_channel_info *const bay_info[] = {
  HWMON_CHANNEL_INFO(fan,
                     HWMON_F_INPUT | HWMON_F_LABEL,
                     HWMON_F_INPUT | HWMON_F_LABEL),
  HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
  HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
  NULL
};

static const struct hwmon_ops bay_hwmon_ops = {
  .is_visible = bay_is_visible,
  .read = bay_read,
  .read_string = bay_read_string,
  .write = bay_write,
};

static const struct hwmon_chip_info bay_chip_info = {
  .ops = &bay_hwmon_ops,
  .info = bay_info,
};

/* --------------------------------------- bay attributes, outside hwmon ABI
 *
 * Bay occupancy and slot power are not sensors, so they are attributes of the
 * I2C client device, the hwmon node's parent (hwmonN/device/), rather than
 * extra attributes on the hwmon device. Raw status bytes whose meaning is not
 * established are diagnostics, not ABI, and live in debugfs instead.
 */

static ssize_t bay_hdd_slots_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result;

  mutex_lock(&state->lock);
  result = bay_refresh_disk_locked(state);
  if (!result)
    result = sysfs_emit(buf, "%u\n", state->hdd_slots);
  mutex_unlock(&state->lock);
  return result;
}

static ssize_t bay_hdd_present_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result;

  mutex_lock(&state->lock);
  result = bay_refresh_disk_locked(state);
  if (!result)
    result = sysfs_emit(buf, "0x%x\n", state->hdd_present);
  mutex_unlock(&state->lock);
  return result;
}

static ssize_t bay_nvme_present_show(struct device *dev,
                                     struct device_attribute *attr, char *buf)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result;

  mutex_lock(&state->lock);
  result = bay_refresh_disk_locked(state);
  if (!result)
    result = sysfs_emit(buf, "%d\n", state->nvme_present ? 1 : 0);
  mutex_unlock(&state->lock);
  return result;
}

static int bay_positions_reported_show(struct seq_file *file, void *unused)
{
  struct bay_data *state = file->private;
  int result;

  mutex_lock(&state->lock);
  result = bay_refresh_disk_locked(state);
  if (!result)
    seq_printf(file, "%u\n", state->positions_reported);
  mutex_unlock(&state->lock);
  return result;
}
DEFINE_SHOW_ATTRIBUTE(bay_positions_reported);

static int bay_status_byte2_raw_show(struct seq_file *file, void *unused)
{
  struct bay_data *state = file->private;
  int result;

  mutex_lock(&state->lock);
  result = bay_refresh_disk_locked(state);
  if (!result)
    seq_printf(file, "0x%02x\n", state->status_byte2_raw);
  mutex_unlock(&state->lock);
  return result;
}
DEFINE_SHOW_ATTRIBUTE(bay_status_byte2_raw);

/* A command, never a reported power state: the controller provides no known
 * readback for this operation. Access is the file mode (0200, root only), as
 * for the SCSI "delete" and PCI "remove" attributes, plus the enable_disk_power
 * opt-in. No fan or disk polling path calls this function. */
static ssize_t bay_slot_power_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
  struct bay_data *state = dev_get_drvdata(dev);
  char action[4], extra;
  unsigned int slot;
  bool on;
  int result;

  if (!enable_disk_power)
    return -EOPNOTSUPP;
  if (sscanf(buf, "%u %3s %c", &slot, action, &extra) != 2)
    return -EINVAL;
  if (slot >= BAY_POWER_SLOT_COUNT)
    return -EINVAL;
  if (!strcmp(action, "on"))
    on = true;
  else if (!strcmp(action, "off"))
    on = false;
  else
    return -EINVAL;

  mutex_lock(&state->lock);
  result = bay_write_slot_power(state->client, slot, on);
  mutex_unlock(&state->lock);
  return result ? result : count;
}

static DEVICE_ATTR(hdd_slots, 0444, bay_hdd_slots_show, NULL);
static DEVICE_ATTR(hdd_present, 0444, bay_hdd_present_show, NULL);
static DEVICE_ATTR(nvme_present, 0444, bay_nvme_present_show, NULL);
static DEVICE_ATTR(slot_power, 0200, NULL, bay_slot_power_store);

static struct attribute *bay_attrs[] = {
  &dev_attr_hdd_slots.attr,
  &dev_attr_hdd_present.attr,
  &dev_attr_nvme_present.attr,
  &dev_attr_slot_power.attr,
  NULL
};

static umode_t bay_attr_is_visible(struct kobject *kobj,
                                   struct attribute *attr, int index)
{
  if ((attr == &dev_attr_slot_power.attr) && (!enable_disk_power))
    return 0;
  return attr->mode;
}

static const struct attribute_group bay_group = {
  .attrs = bay_attrs,
  .is_visible = bay_attr_is_visible,
};

static const struct attribute_group *bay_groups[] = {
  &bay_group,
  NULL
};

/* /sys/kernel/debug/zimacube_bay; an error pointer when debugfs is absent,
 * which every debugfs call below accepts. */
static struct dentry *bay_debugfs_root;

static void bay_debugfs_remove(void *data)
{
  debugfs_remove_recursive(data);
}

/* Diagnostics are optional: a failure here never fails probe. */
static void bay_debugfs_init(struct device *dev, struct bay_data *state)
{
  struct dentry *dir;

  dir = debugfs_create_dir(dev_name(dev), bay_debugfs_root);
  debugfs_create_file("positions_reported", 0444, dir, state,
                      &bay_positions_reported_fops);
  debugfs_create_file("status_byte2_raw", 0444, dir, state,
                      &bay_status_byte2_raw_fops);
  if (devm_add_action_or_reset(dev, bay_debugfs_remove, dir))
    dev_warn(dev, "debugfs diagnostics unavailable\n");
}

/* Registered before hwmon, so devres removes hwmon and drains its sysfs
 * operations before this action sends the final fan command. */
static void bay_safe_on_release(void *data)
{
  struct bay_data *state = data;
  int result;

  if ((!enable_fan_control) || (!state->hwmon_registered))
    return;

  mutex_lock(&state->lock);
  result = bay_write_fan(state->client, safe_percent);
  if (result)
    dev_err(&state->client->dev, "remove fallback failed: %d\n", result);
  else
  {
    state->commanded_percent = safe_percent;
    state->commanded_valid = false;
    state->failsafe_active = true;
  }
  mutex_unlock(&state->lock);
}

static int bay_resume(struct device *dev)
{
  struct bay_data *state = dev_get_drvdata(dev);
  int result = 0;

  mutex_lock(&state->lock);
  state->fan_valid = false;
  state->temp_valid = false;
  state->disk_valid = false;
  if (enable_fan_control && (state->hwmon_registered))
  {
    result = bay_write_fan(state->client, safe_percent);
    if (!result)
    {
      state->commanded_percent = safe_percent;
      state->commanded_valid = false;
      state->failsafe_active = true;
    }
  }
  mutex_unlock(&state->lock);
  if (result)
    dev_err(dev, "resume fallback failed: %d\n", result);
  return result;
}

static DEFINE_SIMPLE_DEV_PM_OPS(bay_pm_ops, NULL, bay_resume);

/* --------------------------------------------------------------------- probe */

static int bay_probe(struct i2c_client *client)
{
  struct device *dev = &client->dev;
  struct bay_data *state;
  int result;

  /*
   * The transfers this driver issues are SMBus block transactions. Refusing
   * here is better than failing every read later on an adapter that cannot
   * do them at all.
   */
  if (!i2c_check_functionality(client->adapter,
                               I2C_FUNC_SMBUS_READ_I2C_BLOCK))
    return -ENODEV;
  if ((enable_fan_control || enable_disk_power) &&
      (!i2c_check_functionality(client->adapter,
                                I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)))
    return -ENODEV;

  if ((fan_count < 1) || (fan_count > BAY_MAX_FANS) ||
      (expected_vendor_id > 0xffff) || (expected_device_id > 0xffff) ||
      (expected_vendor_id < -1) || (expected_device_id < -1) ||
      (minimum_percent < 1) || (minimum_percent > BAY_PERCENT_MAX) ||
      (safe_percent < minimum_percent) || (safe_percent > BAY_PERCENT_MAX) ||
      (watchdog_secs > BAY_WATCHDOG_MAX_SECS))
    return -EINVAL;

  state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
  if (!state)
    return -ENOMEM;

  state->client = client;
  mutex_init(&state->lock);
  i2c_set_clientdata(client, state);

  /*
   * Identify before anything else. This address is on a shared bus, and a
   * driver that binds to whatever answers would be a good way to write fan
   * duties into some other device's registers.
   */
  result = bay_identify(client);
  if (result)
  {
    dev_info(dev, "no backplane controller here (%d)\n", result);
    return result;
  }

  state->fan_count = fan_count;
  state->last_command = jiffies;

  if (enable_fan_control)
  {
    INIT_DELAYED_WORK(&state->watchdog_work, bay_watchdog);
    result = devm_add_action_or_reset(dev, bay_safe_on_release, state);
    if (result)
      return result;

    /* Establish a known duty before publishing pwm1 to userspace. Once
     * userspace writes pwm1, the watchdog tracks that manual command. */
    result = bay_write_fan(client, safe_percent);
    if (result)
    {
      dev_err(dev, "could not apply initial %u%% fan duty: %d\n",
              safe_percent, result);
      return result;
    }
    state->commanded_percent = safe_percent;
    state->commanded_valid = false;
    state->failsafe_active = true;
  }

  state->hwmon = devm_hwmon_device_register_with_info(dev, DRVNAME, state,
                                                      &bay_chip_info, NULL);
  if (IS_ERR(state->hwmon))
    return PTR_ERR(state->hwmon);
  state->hwmon_registered = true;

  bay_debugfs_init(dev, state);

  if (enable_fan_control)
    schedule_delayed_work(&state->watchdog_work, HZ);

  dev_info(dev, "backplane controller at 0x%02x, %u fan(s), fan control %s, slot power %s\n",
           client->addr, fan_count,
           enable_fan_control ? "enabled" : "disabled",
           enable_disk_power ? "enabled" : "disabled");
  return 0;
}

static void bay_remove(struct i2c_client *client)
{
  struct bay_data *state = i2c_get_clientdata(client);

  if (!enable_fan_control)
    return;

  cancel_delayed_work_sync(&state->watchdog_work);

  /* The devres action runs after hwmon's sysfs nodes have been removed. */
}

static const struct i2c_device_id bay_id[] = {
  { DRVNAME, 0 },
  { }
};
MODULE_DEVICE_TABLE(i2c, bay_id);

/* No detect() or address list: this DMI-restricted module instantiates one
 * known address only on the Intel i801 adapter. */
static struct i2c_driver bay_driver = {
  .driver = {
    .name = DRVNAME,
    .pm = pm_sleep_ptr(&bay_pm_ops),
    .probe_type = PROBE_FORCE_SYNCHRONOUS,
    /* added by the driver core after probe succeeds, removed before remove() */
    .dev_groups = bay_groups,
  },
  .probe = bay_probe,
  .remove = bay_remove,
  .id_table = bay_id,
};

static const struct dmi_system_id bay_dmi_table[] = {
  {
    .ident = "ZimaCube Pro",
    .matches = {
      DMI_MATCH(DMI_SYS_VENDOR, "IceWhale Technology CO,.LTD"),
      DMI_MATCH(DMI_BOARD_VENDOR, "IceWhale Technology CO,.LTD"),
      DMI_MATCH(DMI_BOARD_NAME, "ZimaCube Pro"),
    },
  },
  { }
};
MODULE_DEVICE_TABLE(dmi, bay_dmi_table);

static const struct i2c_board_info bay_board_info = {
  I2C_BOARD_INFO(DRVNAME, BAY_I2C_ADDRESS),
};

/* The adapter's parent device lock serializes client creation with i801
 * probe/remove. i2c-core removes the child client if its adapter disappears. */
static DEFINE_MUTEX(bay_bind_lock);
static struct i2c_client *bay_owned_client;
static bool bay_stopping;
static void bay_bind_workfn(struct work_struct *work);
static DECLARE_WORK(bay_bind_work, bay_bind_workfn);

static void bay_attach_adapter(struct i2c_adapter *adapter)
{
  struct i2c_client *client;
  bool bound;

  /* Called with the PCI parent lock held, after looking up the adapter a
   * second time. Never transact from the I2C ADD notifier itself. */
  mutex_lock(&bay_bind_lock);
  if (bay_stopping || bay_owned_client ||
      (!device_is_registered(&adapter->dev)))
  {
    mutex_unlock(&bay_bind_lock);
    return;
  }
  mutex_unlock(&bay_bind_lock);

  client = i2c_new_client_device(adapter, &bay_board_info);
  if (IS_ERR(client))
  {
    if (PTR_ERR(client) != -EBUSY)
      dev_warn(&adapter->dev, "could not create bay controller at 0x%02x: %ld\n",
               BAY_I2C_ADDRESS, PTR_ERR(client));
    return;
  }

  /* The core can return a client without a successfully bound driver. Probe
   * is forced synchronous; a deferred or failed probe must not reserve 0x69. */
  device_lock(&client->dev);
  bound = device_is_bound(&client->dev);
  device_unlock(&client->dev);
  if (!bound)
  {
    dev_warn(&adapter->dev, "bay controller probe did not bind; releasing 0x%02x\n",
             BAY_I2C_ADDRESS);
    i2c_unregister_device(client);
    return;
  }

  mutex_lock(&bay_bind_lock);
  if (bay_stopping)
  {
    mutex_unlock(&bay_bind_lock);
    i2c_unregister_device(client);
  }
  else
  {
    bay_owned_client = client;
    mutex_unlock(&bay_bind_lock);
  }
}

static int bay_match_adapter(struct device *dev, const void *data)
{
  struct i2c_adapter *adapter = i2c_verify_adapter(dev);
  const struct device *parent = data;

  return adapter && (adapter->dev.parent) &&
         ((!parent) || (adapter->dev.parent == parent)) &&
         strstr(adapter->name, "SMBus I801");
}

static void bay_bind_workfn(struct work_struct *work)
{
  struct device *dev;
  struct device *parent;

  /* i2c_for_each_dev() holds i2c-core's core_lock across its callback.
   * Also, i2c_del_adapter() waits for adapter references to drain while
   * i801 holds its PCI parent lock. Keep only the parent reference while
   * waiting for that lock, then find the adapter again under the lock. */
  dev = bus_find_device(&i2c_bus_type, NULL, NULL, bay_match_adapter);
  if (!dev)
    return;

  parent = get_device(dev->parent);
  put_device(dev);

  device_lock(parent);
  dev = bus_find_device(&i2c_bus_type, NULL, parent, bay_match_adapter);
  if (dev)
  {
    bay_attach_adapter(i2c_verify_adapter(dev));
    put_device(dev);
  }
  device_unlock(parent);
  put_device(parent);
}

static int bay_add_notifier_call(struct notifier_block *nb,
                                 unsigned long action, void *data)
{
  struct device *dev = data;
  struct i2c_adapter *adapter;

  if (action == BUS_NOTIFY_ADD_DEVICE)
  {
    adapter = i2c_verify_adapter(dev);
    if (adapter && strstr(adapter->name, "SMBus I801") &&
        (!READ_ONCE(bay_stopping)))
      schedule_work(&bay_bind_work);
  }
  return NOTIFY_DONE;
}

static int bay_remove_notifier_call(struct notifier_block *nb,
                                    unsigned long action, void *data)
{
  struct device *dev = data;
  struct i2c_client *client;

  if (action == BUS_NOTIFY_REMOVED_DEVICE)
  {
    client = i2c_verify_client(dev);
    mutex_lock(&bay_bind_lock);
    if (client && (client == bay_owned_client))
      bay_owned_client = NULL;
    mutex_unlock(&bay_bind_lock);
  }
  return NOTIFY_DONE;
}

static struct notifier_block bay_add_notifier = {
  .notifier_call = bay_add_notifier_call,
};

static struct notifier_block bay_remove_notifier = {
  .notifier_call = bay_remove_notifier_call,
};

static int __init bay_init(void)
{
  int result;

  if (!dmi_check_system(bay_dmi_table))
    return -ENODEV;

  /* Before the driver, since a probe may use it as soon as it registers. */
  bay_debugfs_root = debugfs_create_dir(DRVNAME, NULL);

  result = i2c_add_driver(&bay_driver);
  if (result)
  {
    debugfs_remove_recursive(bay_debugfs_root);
    return result;
  }

  result = bus_register_notifier(&i2c_bus_type, &bay_remove_notifier);
  if (result)
  {
    i2c_del_driver(&bay_driver);
    debugfs_remove_recursive(bay_debugfs_root);
    return result;
  }
  result = bus_register_notifier(&i2c_bus_type, &bay_add_notifier);
  if (result)
  {
    bus_unregister_notifier(&i2c_bus_type, &bay_remove_notifier);
    i2c_del_driver(&bay_driver);
    debugfs_remove_recursive(bay_debugfs_root);
    return result;
  }

  /* The notifier only schedules work. Existing adapters are scanned by the
   * same worker, after any in-progress parent PCI probe finishes. */
  schedule_work(&bay_bind_work);
  return 0;
}

static void __exit bay_exit(void)
{
  struct i2c_client *client;
  struct device *parent;
  bool owned;

  mutex_lock(&bay_bind_lock);
  WRITE_ONCE(bay_stopping, true);
  mutex_unlock(&bay_bind_lock);
  bus_unregister_notifier(&i2c_bus_type, &bay_add_notifier);
  cancel_work_sync(&bay_bind_work);

  mutex_lock(&bay_bind_lock);
  client = bay_owned_client;
  if (client)
  {
    get_device(&client->dev);
    parent = get_device(client->adapter->dev.parent);
  }
  mutex_unlock(&bay_bind_lock);

  if (client)
  {
    device_lock(parent);
    mutex_lock(&bay_bind_lock);
    owned = (bay_owned_client == client);
    if (owned)
      bay_owned_client = NULL;
    mutex_unlock(&bay_bind_lock);
    if (owned)
      i2c_unregister_device(client);
    device_unlock(parent);
    put_device(parent);
    put_device(&client->dev);
  }

  bus_unregister_notifier(&i2c_bus_type, &bay_remove_notifier);
  i2c_del_driver(&bay_driver);
  debugfs_remove_recursive(bay_debugfs_root);
}

module_init(bay_init);
module_exit(bay_exit);

MODULE_DESCRIPTION("ZimaCube drive-bay backplane controller (hwmon)");
MODULE_LICENSE("GPL");
