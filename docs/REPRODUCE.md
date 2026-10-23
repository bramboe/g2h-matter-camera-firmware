# Reproduction via the camera's own update path (self-cleaning app.bin)

At boot the camera installs an `app.bin` from `/customer/app.bin` (or
`/ota/app.bin`) by itself, via `/etc/check_upgrade.sh`: unpack → `copy_files.sh` →
delete `app.bin` → reboot. No signature required. We add only our Matter service;
vendor files (`/local/app.tar.xz`, camera/RTSP/HomeKit) are left untouched, so it
is reversible.

## Self-cleaning — the end result is a clean, updated camera

After a successful install nothing is left behind:

- **Internal:** `check_upgrade.sh` removes `/customer/app.bin` and `/tmp/upgrade`
  automatically and reboots the camera.
- **On the SD card:** our `copy_files.sh` removes the carrier file
  (`/mnt/sdcard/g2h_app.bin`).
- **Result:** a camera with our Matter/Thread service active, without any install
  artifact.

## 1. Build the self-cleaning app.bin (on your computer)

Run this from a checkout of this repository (adjust `REPO` to its path). The init
script `S96g2hmattercamera` is the one written by the install tooling
(`install/g2h_camera_upgrade_live.py`); export a copy of it as `$INIT`.

```bash
REPO=.            # path to this repository
INIT=/path/to/S96g2hmattercamera
B=/tmp/g2h-appbin; rm -rf "$B"; mkdir -p "$B/payload"; cd "$B/payload"
cp "$REPO/app/g2h-matter-camera" camera
cp "$INIT" S96g2hmattercamera
printf '#!/bin/sh\nexit 0\n' > check_coor.sh
cat > copy_files.sh <<'EOF'
#!/bin/sh
# Installs ONLY our Matter service and cleans itself up (internal + SD).
set -e
mkdir -p /customer/g2h-matter-camera /mnt/config/g2h-matter-camera/state
cp /tmp/upgrade/camera /customer/g2h-matter-camera/camera; chmod 755 /customer/g2h-matter-camera/camera
cp /tmp/upgrade/S96g2hmattercamera /etc/init.d/S96g2hmattercamera; chmod 755 /etc/init.d/S96g2hmattercamera
# Clean up the SD carrier (internal /customer/app.bin + /tmp/upgrade is done by check_upgrade.sh itself)
rm -f /mnt/sdcard/g2h_app.bin /mnt/sdcard/app.bin 2>/dev/null || true
sync
EOF
chmod 755 camera S96g2hmattercamera check_coor.sh copy_files.sh
tar -czf "$B/app.bin" .
```

## 2. Preconditions

- A fresh/empty `/customer` (our binary is ~8 MB). On a camera where our app
  already runs, use the network install (`INSTALL.md`), not this path.
- Still to provide per device (unique per camera, so not in app.bin):
  - `/mnt/config/g2h-matter-camera/identity` — the Matter identity.
  - `/mnt/config/g2h-matter-camera/enabled` — empty file to enable autostart.

## 3. Place and install

- SD card as carrier: put `app.bin` on the SD card as `g2h_app.bin`. Copy it once
  on the camera to `/customer/app.bin` (or use `g2h_place_appbin_live.py`).
- Reboot the camera. At boot, `check_upgrade.sh` installs the package, cleans
  everything up (internal + SD) and reboots again. After that our service runs as
  soon as identity and enabled are present.

With `install/g2h_place_appbin_live.py`:
```bash
python3 install/g2h_place_appbin_live.py /tmp/g2h-appbin/app.bin --reboot
# remove a staged file again (cancel):
python3 install/g2h_place_appbin_live.py x --remove
```

## 4. Rollback

`/etc/init.d/S96g2hmattercamera disable` (and optionally remove
`/customer/g2h-matter-camera`). The vendor camera, RTSP and HomeKit were never
changed.
