# Pairing in Home Assistant

The camera pairs as a single Matter device. You pair it once; video, snapshots,
audio, talkback and controls then all run over that one pairing.

Enable the [automatic HA attachment](AUTO-ATTACH.md) once before pairing.

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

Moving video was confirmed on HA 2026.10.2 with Matter server schema 13 using
the project's custom camera integration. The automatic attachment observer
removes its separate manual addition step; the extension still needs to be
installed. HA 2026.10.2 presents it and Matter as linked device pages. A paired node alone does not guarantee
a camera entity in every controller. The exact native HA camera UI version is
not qualified here. See [the evidence matrix](COMPATIBILITY.md); do not re-pair
an already paired device merely because its camera entity is absent.

## Privacy

If you enable privacy mode (via the camera entity in Matter), the app stops video
and audio over Matter. RTSP and HomeKit keep working.
