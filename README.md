# G2H Matter Camera firmware

Custom firmware work for the **Aqara Camera Hub G2H** that turns it into an
**all‑Matter camera** — video (WebRTC), snapshots, microphone, talkback, camera
controls and privacy over a single Matter pairing, with the on‑board JN5189 as an
internal Thread router. It also includes optional **suppression of vendor HomeKit discovery**, applied at
boot. The HAP server itself still runs; RTSP and the Matter pipeline remain available.

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
docs/                Overview + install / pairing / reproduction notes (English)
licenses/            Third-party licenses (FFmpeg LGPL v2.1, Opus BSD) + provenance
```

Set your camera's actual LAN address (no address is assumed):

```bash
export G2H_IP=192.168.1.123   # your camera's IP
```

## Quickstart

1. **Install the Matter app** (adds our service; does not touch the vendor app):
   see [`docs/INSTALL.md`](docs/INSTALL.md) (network install) or
   [`docs/REPRODUCE.md`](docs/REPRODUCE.md) (the camera's own `app.bin` boot
   path). Provide a per‑device Matter identity and the `enabled` flag as described.

2. **Disable the vendor HomeKit (optional):** see
   [`firmware-patch/HOMEKIT-DISABLE.md`](firmware-patch/HOMEKIT-DISABLE.md). A
   size‑preserving one‑byte change to the camera's `_hap._tcp` mDNS service‑type
   string (`_hap`→`_xap`) suppresses normal HomeKit discovery. The HAP server and existing
   connections/pairings are not disabled by that string change. Applied in the
   boot‑time build via the camera's native `hexdump` patch technique, behind a
   cold‑boot guardian that auto‑reverts on failure. Verified live (no `_hap`
   advert after the patch).

3. **Pair over Matter:** see [`docs/PAIRING.md`](docs/PAIRING.md).

## Status / limitations

- Verified on a live G2H: Matter video (40 frames over the SDK WebRTC provider),
  snapshots, microphone, talkback and privacy; RTSP Main 1080p + Constrained
  Baseline 720p preserved; Thread router; HomeKit discovery disabled (no `_hap`
  advertisement; the HAP process still runs, and existing connections are
  not blocked).
- **Home Assistant, native-only (0.5.5 candidate):** use the standard Matter integration and
  Matter Server. Standard audio, full-video and Thread controls are available
  as Mode Select selections on the existing Matter device; no project custom
  component is required. Native motion and three Thread diagnostics also use
  that same device. See [native setup](docs/NATIVE-HOME-ASSISTANT.md) and
  [diagnostics, RTSP and qualification limits](docs/NATIVE-DIAGNOSTICS.md).
  The [native settings candidate](docs/NATIVE-SETTINGS.md) adds WiFi signal,
  night vision, status lamp, date/time overlay and a volume slider on that same
  device, plus extra Thread attributes and a tested snapshot route in the
  existing Matter Server UI.
  The [0.5.6 language candidate](docs/LANGUAGE.md) adds English/Dutch control
  labels and matching private voice banks through one selector on that same
  device. The selected Dutch bank survived an own-app restart; physical speech
  listening remains deferred.
  **HA 2026.11.0b0:** native 1280×720 camera playback was verified on this same
  Matter device, without the project extension. See the exact
  [native-camera qualification](docs/NATIVE-CAMERA.md), including the one-viewer
  limit. HA 2026.10.4 previously lacked the native camera platform; the optional
  historical extension adds a linked device page for those older installations.
- Hardware constraint: the camera binary may only be swapped **during a full boot**,
  never by killing the running camera process (the video hardware does not
  re‑initialise reliably otherwise). The tooling here follows that rule.

## New source candidates

Independent media-session ownership, richer Thread diagnostics, bounded 640×360
snapshots, queued-audio mute fixes, and guarded slot updates are described in
[IMPROVEMENTS.md](docs/IMPROVEMENTS.md). These source changes require hardware
qualification; the historical prebuilt executable remains identified separately.

The installed 0.5.3 [audio, video and Thread controls candidate](docs/CONTROLS.md)
reuses the existing Matter pairing. Full video off/on passed a bounded device
trial: a clean boot suppresses native capture, recording and RTSP tasks; another
boot restores decoded Matter video/audio. The explicit video preference persists
at startup. Both video transitions briefly restart the camera. Audio and Thread
remain separate controls. Audio and Thread disabled preferences passed a
full-boot trial; global OFF is reapplied after late native mute resets. Physical
acoustic silence remains unqualified. Two awake 7,200-frame sessions and idle
reopening passed; broader long-term stability remains unqualified. The historical public
prebuilt is unchanged. See the [current device test report](docs/TEST-REPORT-2026-10-28.md).

## Source builds and release evidence

See [`docs/BUILD.md`](docs/BUILD.md) for pinned inputs, portable build stages,
offline checks and corresponding-source/relinking bundles. Historical packaged
binaries and new source candidates are identified separately; offline tests do
not qualify a new build for installation.

## License

Our code (the `app/` application, `install/` tooling and `firmware-patch/`
scripts/patch) is released under the MIT License — see [`LICENSE`](LICENSE).
Third‑party components bundled or linked by the app keep their own licenses.
Aqara's firmware is **not** included and remains the property of its owner.
