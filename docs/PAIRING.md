# Pairing in Home Assistant

The camera pairs as a single Matter device. You pair it once; video, snapshots,
audio, talkback and controls then all run over that one pairing.

For the standard Home Assistant setup, use only its built-in Matter integration
and Matter Server. No project custom component or automatic attachment is
required. See [native Home Assistant setup](NATIVE-HOME-ASSISTANT.md).

## Steps

1. On the camera, open a pairing window (900 s) with the provided install script,
   or with `kill -USR1 <pid>` on the Matter process. The window changes nothing
   about existing pairings.
2. Make sure the camera's setup code is available. That code belongs to your
   device and is not in this repository; keep it safe.
3. In Home Assistant: **Settings → Devices & Services → Add device → Matter**, and
   enter the setup code. Use that code, not an old radio QR.
4. Once Home Assistant shows the device ("Aqara G2H Thread & Matter Camera"), the
   pairing is complete.

## Display in Home Assistant

The native-only setup keeps one Matter device page. On the qualified firmware,
Home Assistant exposes standard Mode Select controls for audio, full video
capture and the Thread radio. They are Enabled/Disabled selections, not custom
switch entities. Video changes require a full camera reboot.

HA 2026.10.4 has no native Matter camera platform in its discovery mapping, so
it does not create a camera entity or a disabled camera placeholder. A firmware
cannot insert an unsupported-feature banner into HA's UI. The firmware retains
its Matter camera capabilities; this setup does not provide live video,
snapshots, microphone playback or talkback in HA's UI.

The project extension remains an **optional compatibility path** for those
media functions and extra motion/Thread diagnostics. It uses the existing
Matter pairing, but adds a separate linked device page. Do not install or enable
[automatic attachment](AUTO-ATTACH.md) when the goal is one native Matter device
without project integration code. Do not re-pair an existing camera merely
because its camera entity is absent.

## Privacy

If you enable privacy mode (via the camera entity in Matter), the app stops video
and audio over Matter. RTSP and HomeKit keep working.
