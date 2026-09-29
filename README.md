# zimacube-bay

Linux hwmon driver for the ZimaCube Pro drive-bay controller. Both its hwmon
device and kernel module are named `zimacube_bay`. It exposes fan
speeds, controller temperature and bay occupancy. Cooling policy stays in
userspace: a Python daemon can set fan duty through the standard hwmon `pwm1`
attribute. An optional command interface allows an administrator to request
power on or off for one slot.

By default, the driver applies 80% fan duty when it binds. If userspace stops
updating the duty, a watchdog returns it to 80% after 60 seconds. Slot power
control is disabled by default and is never part of the cooling loop.

## Attributes

| Attribute | Meaning |
|---|---|
| `fan1_input`, `fan2_input` | Bay fan speeds in RPM. The second fan is exposed when `fan_count=2` (the default). |
| `fan1_label`, `fan2_label` | `Bay Fan 1`, `Bay Fan 2`; the second label follows `fan_count`. |
| `temp1_input` | Controller temperature in millidegrees Celsius. |
| `temp1_label` | `Backplane`. |
| `pwm1` | Last applied fan duty on the hwmon scale of 0–255. The default 80% is reported as 204. |
| `pwm1_enable` | `0` when the driver holds its fallback duty; `1` after userspace sets a manual duty. |
| `hdd_slots` | Number of HDD positions considered, capped at six. |
| `hdd_present` | Occupancy bitmask; bit *N* corresponds to bay *N*. |
| `nvme_present` | Whether an NVMe device is present. |
| `positions_reported`, `status_byte2_raw` | Additional raw status values for diagnostics. |
| `slot_power` | Optional, write-only slot power command; appears only with `enable_disk_power=1`. |

If a fan tachometer read is zero, the driver waits 150 ms and reads both
tachometers once more. A repeated zero remains visible to userspace. If the
second read fails, the first valid sample is retained.

The bay status attributes are ordinary device attributes because hwmon has no
standard sensor type for bay occupancy.

### Slot power commands

The `slot_power` attribute is hidden unless the module is loaded with
`enable_disk_power=1`. It accepts only a slot index from `0` through `6`
followed by `on` or `off`, for example `0 on`. Writing requires root and
`CAP_SYS_ADMIN`. The command is sent only on an explicit write to this
attribute; the driver never changes slot power during probe, polling,
watchdog fallback, suspend/resume or removal.

The physical mapping of these indices to bays and the electrical effect of
the command have **not** been verified on hardware. A successful write means
only that the bus transfer completed; there is no confirmed power-state
readback. `hdd_present` reports occupancy, not whether a slot is powered.
The companion fan daemon never writes `slot_power`.

An `off` command can make a drive disappear immediately, including while it
is mounted or handling I/O. The kernel driver does not unmount filesystems,
flush applications or detach block devices for you. Before intentionally
turning off a slot, identify its physical disk, stop I/O, unmount its
filesystems and detach the block device (for a SCSI disk, by writing `1` to
`/sys/block/sdX/device/delete`). After turning it back on, the host needs a
rescan to discover the disk again. Do not use this procedure until the slot
index has been validated on non-production hardware.

Writing `pwm1` switches to manual control. Values that map below
`minimum_percent` (default 30%) are rejected. The minimum is a software limit,
not a measured fan stall threshold. Python should write the hwmon attribute;
it must not access the same controller directly while this driver owns it.

`pwm1_enable=0` immediately reapplies the fallback duty. Writing
`pwm1_enable=1` refreshes the watchdog only after a manual `pwm1` write. The
driver does not expose an automatic curve mode.

## Watchdog and parameters

The driver applies `safe_percent` (default 80%) during startup, when the
watchdog expires, on resume and on removal. The watchdog starts tracking a
manual duty when userspace first writes `pwm1`; subsequent writes or
`pwm1_enable=1` keepalives refresh its deadline. After a fallback, userspace
must write `pwm1` again to resume manual control.

| Module parameter | Default | Purpose |
|---|---:|---|
| `fan_count` | `2` | Number of fan inputs to expose (`1` or `2`). |
| `safe_percent` | `80` | Startup and fallback duty. |
| `minimum_percent` | `30` | Lowest allowed manual duty. |
| `watchdog_secs` | `60` | Manual-control timeout; `0` disables the watchdog. |
| `enable_fan_control` | `1` | Set to `0` to disable fan commands, `pwm1` and watchdog. This does not disable explicitly enabled slot power commands. |
| `enable_disk_power` | `0` | Explicitly enable the untested `slot_power` command; independent of fan control. |

The driver requires `1 <= minimum_percent <= safe_percent <= 100` and limits
`watchdog_secs` to 3600. The remaining module parameters are intended for
hardware identification and normally need no adjustment.

## Install

On a supported ZimaCube Pro with matching kernel headers and DKMS installed:

```sh
sudo make dkms
sudo modprobe zimacube_bay
```

The module binds only on matching hardware. It may also load automatically
through its device alias. The Python daemon must write hwmon `pwm1`, rather
than access the controller directly. The hwmon name remains `zimacube_bay`,
so `sensors` still displays `zimacube_bay-i2c-0-69`.

For a read-only inspection, load with
`sudo modprobe zimacube_bay enable_fan_control=0 enable_disk_power=0`.
This leaves fan duty untouched and exposes no slot power command.

`make dkms` installs or updates version `0.4` from `dkms.conf`. `make install`
performs a plain module install without DKMS. `make dkms-purge` removes all
versions of the current `zimacube-bay` package.

When upgrading from `0.3`, DKMS can build and install `0.4` while `0.3` stays
loaded. Stop the Python service and reload `zimacube_bay` to activate the new
labels, then restart the service. Remove the old DKMS registration with
`sudo dkms remove -m zimacube-bay -v 0.3 --all` before rebooting.

### Upgrading from `zimacube-bay-fan`

The old and new module names are different. The old module must be unloaded
before loading `zimacube_bay`, or it may keep the controller at the same bus
address. Stop `zimacube-fan.service`; the old driver returns the fans to its
80% fallback when removed. Install the new `zimacube-bay` DKMS package and
load `zimacube_bay` with `enable_disk_power=0`. Update the Python daemon and
its service unit from [ZimaCubeFan](https://github.com/cyanide-burnout/ZimaCubeFan)
before starting that service again: its `ExecStartPre` must load the new module.
Remove old `zimacube-bay-fan` DKMS registrations before rebooting. List them
with `sudo dkms status -m zimacube-bay-fan` and remove each version with
`sudo dkms remove -m zimacube-bay-fan -v VERSION --all`. The hwmon name and
fan policy are unchanged; this is a module and package rename.

## Current status

The module was built, installed through DKMS and loaded on a ZimaCube Pro
running `6.12.105+deb13-amd64`. Read-only hwmon values, initial 80% duty, a
manual 90% duty, watchdog fallback to 80%, and module unload were checked on
that machine. The companion Python service controlled the fan through hwmon
for over six minutes without a watchdog fallback.

Before the delayed tachometer retry, the second fan reported zero in two of
24 readings at 80% duty. With the retry, there were no zero readings in 60
samples at the same duty. The 0.1 build, which also retains the first
valid sample if the retry fails, was loaded on the Cube. After restarting the
Python service at its 40% idle duty, both fans reported about 1,800 RPM.
These observations do not distinguish a transient tachometer reading from a
brief physical stall. Stop any daemon that accesses the controller directly
while this module owns it.

Version `0.2` was built through DKMS and loaded on the same Cube. Its
`zimacube_bay` hwmon name, initial 80% fallback, two fan inputs and controller
temperature were observed. The updated Python daemon found the renamed hwmon
device and resumed manual fan control at 60% (`pwm1=153`, `pwm1_enable=1`).
Slot power remained disabled: `enable_disk_power=N` and `slot_power` was absent.
No slot power command has been sent or tested on hardware.

Version `0.3` changed module and package naming and is running on the Cube
with the updated Python daemon. Both fans report RPM and the disk-power
interface remains disabled. No slot power command has been sent or tested.

Version `0.4` adds only hwmon labels. It has not yet been loaded on the Cube.

## License

This driver is released under [GPL-2.0-only](LICENSE).
