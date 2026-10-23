# G2H Matter Camera firmware

Custom firmware work for the **Aqara Camera Hub G2H** that turns it into an
**all‑Matter camera** — video (WebRTC), snapshots, microphone, talkback, camera
controls and privacy over a single Matter pairing, with the on‑board JN5189 as an
internal Thread router. It also includes a **permanent disable of the vendor
HomeKit (HAP) AccessoryServer**, applied at boot, while RTSP and the Matter
pipeline keep working.

> **No vendor firmware is redistributed here.** This repo ships *our* Matter app,
> *our* install tooling, and a *patch* that you apply to the camera binary that is
> already on **your own** device. You extract and patch your device's binary
> locally; Aqara's proprietary code is never contained in this repository.

## ⚠️ Read first

- This is unofficial, reverse‑engineered firmware work. **Use at your own risk.**
  It can brick video/streaming if a step goes wrong. There is no warranty.
- Everything here is designed to be **reversible**: the original vendor camera
  stays in `/local/app.tar.xz`, and the HomeKit‑disable install uses a time‑boxed
  **cold‑boot guardian** that auto‑reverts to the known‑good boot if a boot does
  not come up healthy.
- You need root Telnet access to your own camera and basic comfort with a shell.
- Respect the licenses of the third‑party components (see `docs/` and `licenties`
  references); respect Aqara's rights — do not redistribute their binaries.

## What's in here

```
app/                 Our Matter camera application (binary + C++/headers source)
install/             Python tooling to install the Matter app over the LAN (Telnet)
firmware-patch/      HomeKit-disable patch (patch.bin) + ARM patch-helper + method doc
docs/                Dutch install/pairing/reproduction notes (INSTALLATIE / KOPPELEN / …)
```

Set your camera's LAN address once (scripts default to `192.168.1.50`):

```bash
export CAMERA_IP=192.168.1.50   # your camera's IP
```

## Quickstart

1. **Install the Matter app** (adds our service; does not touch the vendor app):
   see [`docs/INSTALLATIE.md`](docs/INSTALLATIE.md) (network install) or
   [`docs/REPRODUCTIE.md`](docs/REPRODUCTIE.md) (the camera's own `app.bin` boot
   path). Provide a per‑device Matter identity and the `enabled` flag as described.

2. **Disable the vendor HomeKit (optional, permanent):** see
   [`firmware-patch/HOMEKIT-DISABLE.md`](firmware-patch/HOMEKIT-DISABLE.md). This
   applies three size‑preserving instruction patches (`CBZ → NOP`) that force the
   vendor's own "AccessoryServer will not start" path, folded into the boot‑time
   build via the camera's native `hexdump` patch technique, behind a cold‑boot
   guardian that auto‑reverts on failure.

3. **Pair over Matter:** see [`docs/KOPPELEN.md`](docs/KOPPELEN.md).

## Status / limitations

- Verified on a live G2H: Matter video (40 frames over the SDK WebRTC provider),
  snapshots, microphone, talkback and privacy; RTSP Main 1080p + Constrained
  Baseline 720p preserved; Thread router; HomeKit permanently off.
- **Home Assistant** renders Matter cameras only from **HA 2026.11** with Matter
  server schema 14. On earlier HA the device pairs but shows no camera entity yet.
- Hardware constraint: the camera binary may only be swapped **during a full boot**,
  never by killing the running camera process (the video hardware does not
  re‑initialise reliably otherwise). The tooling here follows that rule.

## License

Our code (the `app/` application, `install/` tooling and `firmware-patch/`
scripts/patch) is released under the MIT License — see [`LICENSE`](LICENSE).
Third‑party components bundled or linked by the app keep their own licenses.
Aqara's firmware is **not** included and remains the property of its owner.
