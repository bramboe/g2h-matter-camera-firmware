# Candidate qualification — 2026-10-24

This is a development candidate, not a fully qualified stable release.
The retained-session reconnect/restart checks used the preceding 5a799cc8
image with the same broker and media code; the current image adds bounded
Thread table previews to avoid oversized single-string reports.
No setup code, fabric credential, Thread dataset, device-specific address, or
captured camera image is included in this report.

## Exact own application

- App: `0.4.0-candidate`, 8,296,944 bytes.
- App SHA-256: `afe4fbd021d8e3b76c17150ab415c93eaffb8fd701067202211cbf5beaea867d`.
- Broker: 212,980 bytes; SHA-256 `1e7238dea139155d6c8b67fdf4d87a38f784af6a62747fb950c9fcab0ea4736c`.
- Historical prebuilt in `app/` remains identified separately; it is not replaced
  by this candidate merely because compilation succeeded.

## Completed checks

| Check | Observed result |
|---|---|
| Full pinned-source build | SDK, media/crypto/RTC libraries, own app and broker compiled from pinned sources |
| Relink bundle | Relinking extracted objects/libraries produced exactly the app SHA-256 above |
| Source bundle | Manifest validated with runtime license notices and SDK/patch/submodule source-state checks |
| Host tests | 102 tests passed; one optional real-codec test skipped in that default run |
| Separate actual-codec tests | Snapshot sizes, JPEG decode, output cap and sanitizer probes passed in the dedicated host environment |
| First failed switch | Missing shell utility caused an aborted switch; rollback restored the prior selector and broker mode and rebooted into the retained app |
| Corrected slot update | Current app installed in a separate SD slot; original internal app retained; three fabrics preserved |
| Update qualification | Own app ready, fresh Thread state, same host boot and native camera process across each accepted switch |
| Broker video/audio reconnects | Three rounds: 300 video records and 100 audio records each; nonempty RTP data; no native camera restart |
| Own app restart with retained streams | Three restarts; exactly the same two native RTSP socket inodes and broker process retained; host/native process unchanged; fabrics and fresh Thread state retained |
| HA Thread data | Existing Matter pairing used; router, channel 25, neighbor/router counts and raw MAC counters observed in HA |
| HA decoded video | Browser decoded 1280×720 video with advancing media time; later reopen exposed the audio issue below |
| HA snapshot | Standard Matter allocation/capture/deallocation returned a validated 320×180 JPEG, 1,683 bytes; no media included in this report |
| Full Matter interview | Complete read returned 271 attributes after bounding each Thread preview string to 768 bytes; server restart/reload repaired the cached offline state |
| Native motion observation | Exact native image accepted; native reporting enabled; detected-to-clear transitions observed in the HA activity view |

RTP packet receipt is not proof of decoded WebRTC video, audible microphone
quality, or talkback. MAC counters include control/local traffic; they do not
prove how much end-device traffic was forwarded.

## Late qualification findings

- The old broker forwarded video records into the audio-only subscriber. The
  first live session sent video, but the audio reader stopped after one AAC AU;
  subsequent media negotiation failed at audio acquisition.
- A read-only packet count confirmed mixed native tracks. The new broker filters
  out unrelated video and tolerates a bounded audio-decoder startup pause. Eight
  local broker tests pass, including the mixed-track/slow-decoder regression.
  The new broker requires separate full-boot hardware qualification.
- Motion expiry can race an in-flight read. A transition generation ensures that
  the next fresh sample republishes recovery even if its boolean is unchanged.
  HA subsequently showed Clear without recurring age updates.

## Still pending

- Repeated HA/controller disconnects with the corrected broker, plus actual
  640×360 controller snapshots. The initial HA live video and 320×180 snapshot
  passed; they do not establish reconnect reliability.
- Audible microphone/talkback/mute checks and persistent full-boot retest.
- Extended soak, deliberate LAN outage, disconnected update, physical power-cut
  recovery, and a clean build on a second host.

The HA source fixes have host regression tests, but source tests do not replace
the pending live checks above. Global native capture-off remains unsupported:
Matter privacy controls only this app's output and pending talkback.
