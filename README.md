# zimacube-bay-fan

Linux hwmon driver for the ZimaCube Pro drive-bay fan controller. It exposes
fan speeds, controller temperature and bay occupancy. Cooling policy stays in
userspace: a Python daemon can set fan duty through the standard hwmon `pwm1`
attribute.

By default, the driver applies 80% fan duty when it binds. If userspace stops
updating the duty, a watchdog returns it to 80% after 60 seconds. The driver
does not control disk power.

## Attributes

| Attribute | Meaning |
|---|---|
| `fan1_input`, `fan2_input` | Bay fan speeds in RPM. The second fan is exposed when `fan_count=2` (the default). |
| `temp1_input` | Controller temperature in millidegrees Celsius. |
| `pwm1` | Last applied fan duty on the hwmon scale of 0–255. The default 80% is reported as 204. |
| `pwm1_enable` | `0` when the driver holds its fallback duty; `1` after userspace sets a manual duty. |
| `hdd_slots` | Number of HDD positions considered, capped at six. |
| `hdd_present` | Occupancy bitmask; bit *N* corresponds to bay *N*. |
| `nvme_present` | Whether an NVMe device is present. |
| `positions_reported`, `status_byte2_raw` | Additional raw status values for diagnostics. |

If a fan tachometer read is zero, the driver waits 150 ms and reads both
tachometers once more. A repeated zero remains visible to userspace. If the
second read fails, the first valid sample is retained.

The bay status attributes are ordinary device attributes because hwmon has no
standard sensor type for bay occupancy.

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
| `enable_fan_control` | `1` | Set to `0` for read-only operation: no fan commands, `pwm1` or watchdog. |

The driver requires `1 <= minimum_percent <= safe_percent <= 100` and limits
`watchdog_secs` to 3600. The remaining module parameters are intended for
hardware identification and normally need no adjustment.

## Install

On a supported ZimaCube Pro with matching kernel headers and DKMS installed:

```sh
sudo make dkms
sudo modprobe zimacube_bay_fan
```

The module binds only on matching hardware. It may also load automatically
through its device alias. Coordinate with any existing fan-control daemon
before loading it: the daemon must write hwmon `pwm1`, rather than accessing
the controller directly. Upgrade the companion Python daemon to its hwmon
version before running it alongside this module.

For a first read-only inspection, load with
`sudo modprobe zimacube_bay_fan enable_fan_control=0`. This leaves fan duty
untouched.

`make dkms` installs or updates the version in `dkms.conf`. `make install`
performs a plain module install without DKMS. `make dkms-purge` removes all
registered versions of this module before a version change.

## Current status

The module was built, installed through DKMS and loaded on a ZimaCube Pro
running `6.12.105+deb13-amd64`. Read-only hwmon values, initial 80% duty, a
manual 90% duty, watchdog fallback to 80%, and module unload were checked on
that machine. The companion Python service controlled the fan through hwmon
for over six minutes without a watchdog fallback.

Before the delayed tachometer retry, the second fan reported zero in two of
24 readings at 80% duty. With the retry, there were no zero readings in 60
samples at the same duty. The current build, which also retains the first
valid sample if the retry fails, is loaded on the Cube. After restarting the
Python service at its 40% idle duty, both fans reported about 1,800 RPM.
These observations do not distinguish a transient tachometer reading from a
brief physical stall. Stop any daemon that accesses the controller directly
while this module owns it.

## License

This driver is released under [GPL-2.0-only](LICENSE).
