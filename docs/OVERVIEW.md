# G2H Matter Camera — overview

One Matter device from the Aqara Camera Hub G2H: video, snapshots, microphone,
talkback and camera controls, all over a single Matter pairing. The JN5189 stays
a standalone Thread router (FTD), not a Border Router. Experimental software, not
a certified product.

## What works (tested over Matter)

- **Video** — H.264 720p over WebRTC (Offer/Answer over Matter CASE, DTLS/SRTP).
- **Snapshots** — JPEG 320×180, shared ingest so live video does not stutter.
- **Microphone** — Opus mono 48 kHz, transcoded from the camera's AAC source.
- **Talkback** — half-duplex, Opus in, played through the camera's speaker
  (~0.5–1 s latency).
- **Controls**
  - Standard Matter attributes: night vision (Off/On/Auto), status LED, HDR (WDR),
    speaker volume, mute microphone/speaker.
  - **Privacy mode** (SoftLivestreamPrivacy): the app stops video *and* audio over
    Matter; the vendor stream, RTSP and HomeKit are untouched. Reversible.
  - G2H vendor cluster `0xFFF1FC11`: brightness, on-screen timestamp (OSD),
    speaker volume. Standard controllers do not display this cluster.
- **Thread** — G2H vendor cluster `0xFFF1FC10`: role, channel, network name, etc.

## What does not work (yet) / is missing

- **Display in Home Assistant** — HA renders Matter cameras only from HA 2026.11
  with a Matter server of schema 14. After that the camera appears automatically
  in the existing Matter device; no re-pairing needed.
- **Image mirror/rotate** — no safe live setting; only a config key that requires
  restarting the camera program. Not built.
- **Recording, global hard-privacy, Wi-Fi over Matter** — not applicable.

## Known side effect

If the Matter app stops after an audio session (on update, restart or reboot),
Aqara's camera program restarts once. RTSP and HomeKit come back on their own. In
normal use the app does not stop.

## Repository layout

- `app/g2h-matter-camera` — the ARM binary (see `SHA256SUMS`).
- `app/src/` — our own source code (the Matter app; no vendor code).
- `install/` — scripts to install, recover and host-check, with the helper modules.
- `firmware-patch/` — the HomeKit-disable patch + ARM helper + method doc.
- `licenses/` — LGPL licenses and source references for FFmpeg and Opus.
- `docs/INSTALL.md` — step-by-step install.
- `docs/PAIRING.md` — pairing the camera in Home Assistant.
- `docs/REPRODUCE.md` — the camera's own `app.bin` self-install path.
- `SHA256SUMS` — verification hashes.

## Licenses

The app links statically against FFmpeg (LGPL v2.1) and Opus (BSD). See
`licenses/`. The full, rebuildable sources of FFmpeg 8.1.3 and Opus 1.6.1 belong
to a distributed release; provenance is in `licenses/herkomst.json`.
