# Installing and updating

The update tools operate on an **already configured** G2H: host firmware 2.2.7,
working project Matter service, persistent per-device identity/state, and an
independently configured JN5189 Thread router. They do not convert an untouched
stock camera or flash its radio. The experimental first-install packaging is
separate: [REPRODUCE.md](REPRODUCE.md).

## Check and back up

Use Python 3.12+ and the camera's actual address. Passwords are prompted before
connecting and never stored in the source tree.

```sh
export G2H_IP=192.168.1.123
python3 install/g2h_host_check_live.py
python3 install/g2h_backup_live.py
```

Backups are private, outside the repository by default. Every file is checked
against its remote size/MD5 and a local SHA-256. A backup is incomplete if the
host, vendor process or Matter app changes during the download. Matter fabric
keys, Thread datasets and camera recordings are not copied by these tools.

## Candidate update

Build candidates using [BUILD.md](BUILD.md); do not assume source additions are
already present in the historical `app/g2h-matter-camera` binary. Close Matter
live views before updating. For the first broker migration, the old app must not
have opened any native RTSP sessions since boot. If the tool refuses this
condition, reboot normally and retry before opening a Matter live view.

```sh
python3 install/g2h_camera_upgrade_live.py \
  --image builds/source/app/g2h-matter-camera \
  --broker builds/source/app/g2h-media-broker
```

An already installed broker must match the supplied broker exactly. A changed
broker requires a separate full-boot migration; the app updater deliberately
refuses to replace a broker that owns native RTSP sessions. See
[BROKER-MIGRATION.md](BROKER-MIGRATION.md).

This creates a fresh backup, stages and verifies a **separate executable slot**,
and leaves the original app intact. It requires a 1 MiB free-space margin and
adequate temporary RAM. It refuses deletion of the only working copy. When
internal storage cannot hold a separate slot, an already-mounted SD card can
hold it instead:

```sh
python3 install/g2h_camera_upgrade_live.py \
  --image builds/source/app/g2h-matter-camera \
  --broker builds/source/app/g2h-media-broker --storage sd
```

The SD card must remain mounted while that slot is selected. This is a network
update to a separate SD slot, **not** a drop-in boot-time SD installer. Do not
format/remove the card without selecting the retained internal app first.

An on-camera guardian rolls an unconfirmed transaction back after 180 seconds,
independently of the installer connection. Interrupted transactions also select
the previous app on the next boot. On the first migration, rolling back to an
old direct-RTSP app performs a full reboot to clear broker-owned native sessions.
Later broker-compatible app restarts retain those native sessions. The vendor
camera, shared monitor/watchdog and radio are never stopped by the app updater.

`CANDIDATE_READY_IDENTITY_PRESERVED` proves the startup qualification window,
unchanged fabric count, correct running executable, broker mode and unchanged
host/vendor lifetime. It does **not** prove decoded video/audio, hardware privacy
or a long soak test. Perform the release checks in [COMPATIBILITY.md](COMPATIBILITY.md).

A fresh backup from the same process lifetime can optionally be reused for up
to ten minutes with `--verified-backup /private/path/to/g2h-backup`; its identity,
local data and current executable checksum are checked again.

## Explicit rollback

Use the exact backup directory printed by the updater:

```sh
python3 install/g2h_camera_service_recover_live.py --backup /private/path/to/g2h-backup
```

Recovery checks the retained executable against that backup; it does not guess a
historical build or overwrite an unknown binary. A rollback to legacy software
restores its broker setting and reboots. Keep the backup on another computer;
the camera's SD card alone is not an independent recovery copy.

## Home Assistant

[PAIRING.md](PAIRING.md) describes the existing Matter pairing. The source under
`home-assistant/custom_components/g2h_matter_camera` includes the project camera
extension and adds diagnostics/motion to that same existing device. Upgrade its
files and restart/reload HA as appropriate; no new pairing or Thread dataset is
required. See [THREAD.md](THREAD.md) and [IMPROVEMENTS.md](IMPROVEMENTS.md) for the
scope and qualification of these candidates.
