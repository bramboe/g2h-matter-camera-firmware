# Device controls trial — 2026-10-26

This is a bounded candidate test, not a stable/privacy release qualification.

## Exact installation

- Own Matter app: `0.5.1-controls-candidate`, 8,307,952 bytes, SHA-256
  `8b8446afa8596a93f8d4477677ce2b27f9076fed1397a626ec39b0275a02c245`.
- Broker unchanged: SHA-256
  `5c1940064665258cb695f81e1279287316aece034331fed2e0930c8cca8e4399`.
- Native image unchanged. No radio flash, GPIO/watchdog/monitor intervention,
  native warm restart, fabric edit or dataset/key read/write.
- Fresh verified private backups retained before each installation. The first
  0.5.1 attempt did not meet the readiness window and rolled back. A second
  attempt with the identical image passed the same 30-second readiness test.
  The first failure's precise cause was not established.

## Transport compatibility

Vendor-only writes were not supported by Matter(.js) Server 1.4.0's model-based
write handler. The 0.5.0 HA attempt failed without changing native mutes.
The corrected build adds standard Mode Select servers: endpoint 1 Camera audio
and endpoint 0 Thread radio, Disabled=0 and Enabled=1. HA's core Matter integration
automatically created labelled mode selectors under the existing Matter device.
The companion 0.5.1 switch implementation invokes ChangeToMode and still checks
independent observed vendor/radio attributes. See [CONTROLS.md](CONTROLS.md).

The companion was updated from 0.4.1 through the 0.5.0 trial to 0.5.1, with
verified private component backups. HA was restarted after checking that no
automations/scripts were running. The **Geluid** switch subsequently changed
both native mute flags to 1 and back to 0; the **Thread** switch reached the
observed disabled state and returned to enabled/router. Original audio flags
and volume were confirmed restored before cancelling the guard. These were real UI operations through the original HA
fabric, not optimistic entity values. A separate local restoration guard
protected this HA trial as well.

## Guarded device operations

A detached on-device restore timer was exercised against the unchanged original
values before any mutation. A separate five-minute restoration guard protected
the actual off/on operations.

- Audio native flags: microphone/speaker 0/0 → 1/1 → 0/0.
- Speaker volume stayed 80 throughout.
- Thread observed enabled → disabled → enabled, eventually router again.
- A Matter read over Wi-Fi succeeded while Thread was disabled.
- Boot/native process identity stayed unchanged; the existing broker was retained.
- Temporary test fabric removed; the original three fabrics remain.
- No off/on reboot-persistence or acoustic-silence claim is made from flag readback.

## Media after the device trials

An independent Matter controller then completed CASE signalling and DTLS/SRTP,
received and decoded 120 H.264 frames and received 345 Opus packets. Decoder
exit was zero, audio decode errors did not increase, no media was saved and no
talkback signal was played. The temporary fabric was removed, the native
process/boot were unchanged, and Thread remained router. This short test is not
a duration/stability qualification.

## Offline validation and remaining work

151 host tests: 150 passed, one optional codec test skipped. Pinned SDK and
ARM app builds passed; source-layout checks and offline manifest verification
passed. Physical microphone/speaker silence and reboot persistence remain
unqualified. Full camera image-off, including native RTSP and snapshots, is
not implemented; the native video setting only gates motion and is unsuitable.
The earlier long-duration media/control stalls remain open. No stable tag or
merge is created.
