# Installation

The proven install method is over the network (Telnet) from a computer. A real
"drop it on the SD card and the camera installs itself" procedure has not been
established on this camera; see the bottom.

## Requirements

- The camera on the same network, reachable over Telnet, and the camera password.
- Python 3 on the computer.
- This repository, with `app/g2h-matter-camera` and the `install/` directory.
- Your camera's IP: `export CAMERA_IP=192.168.1.50` (scripts default to that).

## Install

1. First check the host (read-only, changes nothing):
   ```bash
   python3 install/g2h_host_check_live.py
   ```
2. Install the app. `/customer` has too little room for two copies, so use
   `--in-place` (the previous build serves as fallback):
   ```bash
   python3 install/g2h_camera_upgrade_live.py --in-place
   ```
   Expect `UPGRADED_APP_READY`. A one-time
   `WARNING_VENDOR_CAMERA_RESTARTED` is normal.
3. Recover after a failed install, or to roll back:
   ```bash
   python3 install/g2h_camera_service_recover_live.py
   ```

Before every flash the scripts make a fresh backup, verify hashes, and restore the
previous build on error. They do not touch the radio, the vendor camera config, or
the boot process beyond replacing the app under `/customer`.

## Pairing

See [`PAIRING.md`](PAIRING.md).

## About the SD card

Placing a file on the SD card is **not** a proven install path on this camera: the
camera's boot does not read an install package from the SD card. An SD-card install
would require changes to the camera's boot, which risks an unusable device. That is
why the network method above is the recommended, tested reproduction method. If you
still want an SD-card route, discuss it first — that is separate research with a
fallback plan.
