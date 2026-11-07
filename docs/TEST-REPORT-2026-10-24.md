# Candidate qualification — 2026-10-24, updated 2026-10-25

This is a development candidate, not a fully qualified stable release.
The retained-session app restart checks used the preceding 5a799cc8 image
and first broker. Three decoded HA reopen cycles used app afe4fbd0 and the
corrected broker below. The current app has the same media code and additionally
removes periodic motion-age dirty notifications; its guarded startup qualification passed with unchanged host/native lifetime
and all three existing fabrics retained. On 25 October the exact current app
also delivered decoded HA video and two independently decoded 640×360 downloads
while the same host/native process lifetime and fresh Thread state were retained.
No setup code, fabric credential, Thread dataset, device-specific address, or
captured camera image is included in this report.

## Exact own application

- App: `0.4.0-candidate`, 8,296,752 bytes.
- App SHA-256: `e72108001d1f6a4379aad434ef9179ef7bc810792ef1cf122f5af917072549ef`.
- Broker: 212,980 bytes; SHA-256 `1e7238dea139155d6c8b67fdf4d87a38f784af6a62747fb950c9fcab0ea4736c`.
- Historical prebuilt in `app/` remains identified separately; it is not replaced
  by this candidate merely because compilation succeeded.

## Completed checks

| Check | Observed result |
|---|---|
| Full pinned-source build | SDK, media/crypto/RTC libraries, own app and broker compiled from pinned sources |
| Relink bundle | Relinking extracted objects/libraries produced exactly the app SHA-256 above |
| Source bundle | Manifest validated with runtime license notices and SDK/patch/submodule source-state checks |
| Host tests | 111 tests passed; one optional real-codec test skipped in that default run |
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
| Corrected broker full boot | New broker checksum and changed host boot verified; three fabrics, selected app, native image hash and fresh Thread router preserved |
| Corrected broker HA reconnects | Three 1280×720 decoded HA sessions; explicit peer-ended idle states between sessions; 6,068 video frames and 15,357 Opus packets sent with zero AAC decode errors and unchanged host/native lifetime during the test |
| Real SDK motion reporting test | ARM executable linked with the production SDK passed age-only/no-change and detection/expiry/recovery/disabled transition assertions on the camera; host/native process unchanged |
| Final app HA media | Current app decoded 1280×720 video with advancing browser media time; two 640×360 JPEG downloads decoded independently with FFmpeg while live video continued; peer-ended idle observed afterward, total 4,171 video frames / 10,660 Opus packets, zero AAC decode errors |
| HA microphone listening check | On 25 October the user enabled audio in the G2H HA camera view and confirmed clear, audible microphone sound; talkback and mute were not part of this check |
| Final motion-only app slot | Exact current app accepted after the guarded startup window; native camera, corrected broker and host lifetime retained |
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
  Full-boot qualification and the three HA reopen cycles above passed after
  this correction.
- Motion expiry can race an in-flight read. A transition generation ensures that
  the next fresh sample republishes recovery even if its boolean is unchanged.
  HA subsequently showed Clear without recurring age updates. The strict two-second
  freshness limit still occasionally causes genuine unavailable/recovered transitions
  under load; this remains a qualification issue, not a fabricated clear sample.
- Matter.js attribute reads encode snapshot structs with numeric TLV tags, unlike
  typed command responses. The HA selection now accepts both bounded shapes and
  keys its short cache by requested size preference. A cold worker capture gets
  one delayed retry on the same allocation; errors still release the stream.
  Both 640×360 controller downloads passed independent JPEG decoding.
- The two JPEG fixtures in `tests/fixtures/` are generated synthetic test patterns,
  not device-derived images. Tests exercise actual JPEG dimensions, older-device
  fallback, malformed capability data, size-aware caching and bounded retry cleanup.

## Still pending

- Occasional motion availability transitions under load at the strict two-second
  freshness deadline. Repeated unchanged ages no longer create motion state updates.
- Audible talkback and mute quality checks. The user confirmed clear microphone
  audio in HA on 25 October; this does not qualify the reverse audio path or mute.
- Extended soak, deliberate LAN outage, disconnected update, physical power-cut
  recovery, and a clean build on a second host.

The HA source fixes have host regression tests, but source tests do not replace
the pending live checks above. Global native capture-off remains unsupported:
Matter privacy controls only this app's output and pending talkback.
