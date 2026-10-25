# Vendor media boundaries and follow-up validation

This engineering note separates static findings, historical device observations,
and proposed changes. It contains no vendor executable or decompiled source.
The offline investigation on 2026-10-24 used the owner's post-extraction G2H
2.2.7 camera executable, SHA-256
`db757b6272e0b563671b4629cf02f86ec9dc9b13d1b7f83bbc0155538dd599f8`.
No camera connection or hardware write was performed for this investigation.

## RTSP ownership and audio shutdown

Historical device trials observed vendor camera restarts after closing an audio
ingest session on `/13`. Setting up both tracks did not eliminate the restart;
neither did the tested graceful-close variant. An audio-only graceful-close
variant requires its own hardware test; its existence is not evidence of a fix.

Static analysis establishes these boundaries:

- RTSP TEARDOWN and TCP disconnect reach common synchronous session cleanup.
  Cleanup requests worker cancellation, waits for a worker flag, then frees the
  session. An unbounded native frame read can hold the entire RTSP event loop.
- Worker exit clears the flag before cancelling/joining a shared global IR
  supervisory thread. Every RTSP session uses this global thread identity.
  Shared lifetime/race problems are candidates, not proven crash causes.
- The native RTP TCP writer uses `MSG_NOSIGNAL` and bounds transient send retries.
  A theory that RTP writes simply terminate the process through SIGPIPE is not
  supported by this path.

A persistent local media broker can mitigate the app-update interruption:
the broker owns long-lived native RTSP ingests, and Matter app instances own
only subscriptions. Closing a subscription must never close an ingest. Give
each subscriber bounded queues; evict a slow subscriber without stopping the
native source. Restrict the local socket to the intended owner, validate the
peer and all length fields, and discard incomplete/expired media.

This is a lifecycle mitigation, not a repaired native destructor. Broker
shutdown still reaches the native close path. A broker upgrade must therefore
have a separate recovery procedure and must not be smuggled into an ordinary
Matter-app restart. Never kill/reinitialize the vendor hardware process as an
ordinary update step.

Required hardware checks: repeated Matter subscriber restart/upgrade with a
constant broker PID, vendor PID and boot ID; concurrent audio/video and external
RTSP; bounded memory/CPU under slow clients; subscriber recovery after failure;
and an explicit broker shutdown/recovery trial with rollback available.

## Privacy: distinguish delivery from capture

The existing Matter privacy gate stops Matter media delivery. It does not by
itself stop the sensor, native RTSP, or native HomeKit capture.

The vendor loopback debug interface accepts NUL-terminated commands of the form
`<module> <setting> [integer]`. Known candidates include `video on` (0/1) and
`audio in_mute` (use 0/1 even though the vendor registration allows 0..100).
These are persistent settings; a trial must save and restore their exact
previous values.

**Do not label `video on 0` a verified global privacy switch.** The setter's
callback only extends a motion-suppression timer when enabling video. In the
examined shared video callback, the stored `video_on` value gates motion
reporting; H.264 publication does not use that gate. This is evidence against
assuming that this property stops RTSP or turns off the sensor.

**A debug setter acknowledgement is not proof of effect.** The debug parser
formats its success-shaped reply without checking the setting writer's result.
The microphone-mute callback also discards the underlying driver error. Numeric
readback is necessary, but actual decoded audio must be checked before claiming
microphone privacy.

A safe temporary experiment needs an independent local restore guard already
running before any setting write. Save original values; gate Matter delivery
and flush pending media first; apply one setting at a time; read back; measure
the actual native video/audio effect without retaining household recordings;
restore and verify on success, error and client disconnect. A test failure must
leave privacy status unknown/error, not falsely report successful global
privacy. Preserve Thread operation throughout. Do not persist a new global
privacy option until its off/on and reboot behavior has been demonstrated.

Blocking an RTSP port or dropping broker subscriptions is a network delivery
restriction. It is not evidence that the sensor or microphone stopped capturing,
and must be described accordingly.

## Motion: real producer exists, external observation remains unverified

The native YUV callback runs the existing motion detector over enabled regions.
A positive result can produce a gateway report with resource `3.2.85`, value 1.
Reports depend on the configured alarm schedule, video flag, valid wall clock
and interval throttling. This is a positive event feed, not a continuous
occupancy measurement. The report path uses an existing gateway TCP session;
no independent externally supported subscription API has been established.

The same callback drives the native HomeKit motion state through its signal
handler. The registered `homekit_motion_detected_get` function returns the
process-local `g_hk_motion_detected` integer. However:

- The HomeKit motion state is gated by native camera-active/manual-disable
  state. It can stay false while the native detector still sees movement.
- The debug function dispatcher does not turn the callback's integer return
  into a response value; this getter does not fill the response buffer.
  Sending `function homekit_motion_detected_get` is therefore not a verified
  polling API.
- Changing the advertised service string `_hap._tcp` to `_xap._tcp` leaves
  native HomeKit internals running. It does not remove this producer, but it
  also does not guarantee the producer is enabled or fresh.

Do not map an unavailable/stale feed to “no motion” or occupancy false. Before
adding a sensor endpoint, verify a passive event source with controlled movement
and stillness, distinguish source availability from event expiry, and publish
motion semantics explicitly. A finite hold time after a positive event is an
application policy, not a measured absence of motion. Keep the endpoint disabled
until the source and recovery behavior have been verified.

## Read-only observer candidate and wire contract

`app/src/g2h_motion_observer.hpp` now implements a candidate read-only observer
for the exact currently backed-up executable SHA-256
`f942639021769407028aa41e1064d794dd86f07ffab1442a91112c2aa678c799`.
It verifies process identity, root ownership, ELF layout and the whole executable
hash before reading only the four-byte held-motion field and one-byte enable
field. Both fields must lie in private read/write mappings. This vendor's BSS
may be `rwxp`; that is accepted only alongside all the exact-image checks.
Shared or unreadable mappings are rejected. Executable hashes are cached by
PID, start ticks and file metadata; a restart or changed image requires a fresh
hash. The first hash has a 12-second worker deadline. Transient hashing failures
have a 30-second retry cooldown; they are not permanently cached as a wrong
image. No permission changes or ptrace attachment is attempted.

`g2h_motion_cluster.hpp` defines development vendor cluster `0xFFF1FC12` on
endpoint 0, revision 1, using the camera's existing Matter identity:

| Attribute | Type | Meaning |
| --- | --- | --- |
| 0 | Boolean | A held-motion value is known and within the advertised freshness limit |
| 1 | Nullable Boolean | Observed native held-motion state |
| 2 | Nullable Boolean | Observed native producer enable flag |
| 3 | String | Static status/failure reason |
| 4 | String | `vendor-vision-held-motion` |
| 5 | Nullable uint32 | Sample age in milliseconds |
| 6 | uint16 | Freshness limit: 4000 milliseconds in the new source (earlier images: 2000) |

A verified disabled producer yields fresh=false, motion=null, enabled=false;
it does not yield motion=false. After the advertised freshness limit, both measured values expire.
Unsupported images, permission failure and incomplete reads also yield unknown.
The observer must run on a dedicated worker at most once per second, not the
Matter event loop. This code and the isolated ARM cluster compile have offline
tests; actual observation of movement and clearing still requires a device trial.

The pinned Occupancy Sensing revision-5 SDK model includes the Vision feature
but retains mandatory legacy sensor-type attributes with only the older sensor
enum values. A maintainer explains that newer modalities can legitimately use
the legacy default for compatibility while the feature map identifies the real
modality. See the [Home Assistant maintainer discussion](https://github.com/home-assistant/core/issues/164839).
Therefore this is a legacy compatibility question, not proof that Vision cannot
use the standard cluster. The current implementation deliberately uses an
explicit vendor motion cluster until standard conformance and controller behavior
are verified; it does not advertise a physical PIR detector or continuous
occupancy. A compatible HA extension is needed to render this vendor cluster.

## Evidence ledger

REA Evidence IDs refer to the exact original digest above; they are not runtime
observations. Full evidence is retained privately outside this public repository.

| Finding | Evidence |
| --- | --- |
| RTSP shared cleanup | `ev_9378732c89f534a7921fc60deacf6faf360d1e0e3152333d311043d0bc50623f` |
| Worker read/exit and shared IR cleanup | `ev_54e130c3a049cb8809c5f8fa8bc06dc7f80b45668e9f834c7d04d81ae45939f1` |
| Bounded RTP TCP writer with MSG_NOSIGNAL | `ev_645349e93ba24bb9eab08ff2c0ce752f1a8409eb4b36775d38891398f3ee9655` |
| Video-on callback | `ev_ba99bf3e94a5f359b3bdaac88f76d13659268d47555872a69ebe26cfdfa38629` |
| Native video publication and motion branch | `ev_062f0cdea33b15589d7e0040e3ce43ad26ae6a8181f14b5e445ec6c3f5e3f2dc` |
| Debug dispatcher and unchecked setter acknowledgement | `ev_3ca09654c4a008af247cc99b29a0eb116903c1389b2375697b40c3812dc8a8c5` |
| Microphone callback discards driver result | `ev_e2476c2a5acb2761083695a86221669f2969ae83b930f9793daabfbefb4b5655` |
| Underlying microphone driver mute | `ev_84d5c8f617e72f31585eabca7be8e060538ebe83f7c912f5b934c65a0a5367e7` |
| Motion getter and command registration | `ev_6f1190b478e98b59abdeedc8a5038273bd3c0098ca936471a8b1719180468362`, `ev_2df4bd3cff43ba2082c5248e822b56ecbffb61cba3641dda88054708d32bdf77` |
| HomeKit motion signal and enable gates | `ev_cc87739d4522f665cf98fc6d525207782a94e7fff68d4b11c4f8592cd5ff4e35`, `ev_7783176e100aa4688aae4bc6ccbb3607bb5ff5ff9b6ddf43354b9e2e66f911e0` |
| Positive resource report and existing gateway session | `ev_f42037da3325fad5e716595581835918714dfbcdc883470ddc8c02ee00709054`, `ev_46701b4c276c0d0b593111b8931ca0f35dde4db21003916ec3dc774376a10dea` |

Native function recovery includes merged/noncontiguous bodies and decompiler
artifacts. Instruction-level checks and a hardware trial are required for any
future native patch. This note does not establish a working global privacy
switch or a hardware-verified motion endpoint.
