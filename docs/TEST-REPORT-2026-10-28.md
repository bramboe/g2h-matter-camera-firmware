# Video boot control candidate — 2026-10-28

## Build and app-only qualification

The 0.5.2 candidate adds an optional standard Matter Mode Select control on
endpoint 2. It is registered only when a separately installed, private native
profile/backend is present. Existing endpoint 0 Thread and endpoint 1 audio
controls and the camera identity are retained. The utility endpoint uses a
vendor development device type, not a plug or another camera.

App SHA-256: `40ce7d7ac1254acf7db5428569de7902d4951260cb90734de19c3d4071be8a58`.
The existing broker is retained unchanged:
`5c1940064665258cb695f81e1279287316aece034331fed2e0930c8cca8e4399`.

The app passed a guarded 60-second continuous device qualification: 30 recorded
allowlisted observations, three original fabrics, fresh attached Thread status,
healthy media source, progressing status clock, unchanged native process and
unchanged host boot. A private verified backup was taken before the update.

The host suite ran 175 tests: 174 passed and one optional codec test skipped.
Tests include actual local native-setting peers, UART peers, privacy races,
boot-service failure paths and bounded optional HA RPCs. Passing these tests
alone does not establish physical capture or acoustic privacy.

## Chosen video transition

Runtime native sensor stop/start can restore the ISP frame counter while
leaving established video delivery broken. That route is not used by the
video control. Both transitions instead use a full camera boot, preserving
Matter fabrics and the Thread dataset. A verified native process wrapper
blocks capture, recording and RTSP tasks before they start when the explicit
video preference is off. No vendor executable is written by this backend.

The app immediately clears its own video/snapshot caches and ends its active
Matter video session before queuing the reboot. Native image-setting writes
are blocked until capture is physically observed on. Audio and Thread remain
separate controls. CurrentMode is based on the preference plus kernel ISP,
VI and VENC observations; a queued request is never reported as success.

An independent temporary installation guardian can restore the original
startup configuration during qualification. It must be removed/disarmed before
committing the user-facing switch: production privacy must not time out and
silently turn video back on.

## Device transition qualification

Before OFF, a temporary CASE controller decoded 120 H.264 frames and received
413 Opus packets over the encrypted Matter media path. No media was saved.

The standard endpoint 2 ChangeToMode(0) was accepted. Following a full boot:

- ISP frame counter remained zero, including a further five-second observation.
- VENC channels 0 and 1 had Create=0 and Start=0; VI channels 0 and 1 Enable=0.
- No native record_task thread or SD-recording file descriptor was present.
- No RTSP TCP listener was present on port 554.
- The exact helper was mapped into the exact verified native image.
- The explicit off preference remained stored; Matter reads succeeded.
- Original fabrics plus only the temporary test fabric remained, and Thread
  was freshly observed as router.

The standard ChangeToMode(1) was then accepted. After the next full boot,
ISP frames advanced again. The temporary CASE controller decoded another 120
H.264 frames and received 336 Opus packets over the encrypted Matter media path,
with decoder exit zero and no saved media. Both CurrentMode reads succeeded.

The test controller removed only its own fabric, returning the original count
to three. The wrapper/profile and explicit enabled preference were committed;
the timed guardian was disarmed and its boot hook removed. No automatic privacy
restoration remains in the installed control. Thread was fresh/router and the
source was healthy after the final boot. The vendor image and original media
broker remained byte-identical. No radio flash, dataset/key inspection or
native warm restart was performed.

This is a bounded device qualification of both transitions. It does not establish
long-duration stability or acoustic silence. The historical public prebuilt
executable is unchanged; no stable release tag is created.

## Home Assistant update

The companion 0.5.2 switch, metadata and diagnostics files were installed with
byte verification after private rollback copies were saved. HA reported no
running automations/scripts before its controlled restart. All three labelled
switches appeared under the existing companion camera device.

The actual HA Video switch was then exercised OFF and ON, with new host boot
identities verified in each direction. OFF again showed zero ISP/VI/VENC
activity. ON showed ISP 4007→4049 and active channel 0. Original fabric count
remained three and Thread returned to router. The temporary installation
recovery hook remained absent during these production-control operations.

The HA browser accessibility text retained “Unable to play media” even while
the video element was playing. Independent read-only media-element observations
confirmed 1280×720, readyState=4, paused=false, error=null and advancing playback
time 90.596→101.841 seconds. The live view was then closed normally. No household
video or snapshot was saved. A separate idle source fault was observed after
boot and later cleared on normal source use; this does not close the previously
known intermittent media-source stability issue.

Final HA controls all show ON, the camera is idle and Thread is router. The
additional HA camera-platform/configuration tests passed (79 tests). Private
terminal recovery instructions and exact installed artifact hashes are retained.
Long-duration stability, physical acoustic silence and Thread-disabled reboot
persistence remain outside this bounded qualification.

Final read-only health check confirmed healthy idle Matter source, three fabrics,
fresh Thread router, video enabled and RTSP /14 DESCRIBE 200 with a video track.
The optional automatic privacy restoration hook remained absent.

## Follow-up qualification and native mute reset

A subsequent 368.5-second independent stream decoded all 7,200 requested H.264
frames and received 17,969 Opus packets. This exceeded the earlier observed
100–284 second failure window without reproducing that failure in this session.
It is a bounded observation, not a general stability guarantee.

A combined audio/Thread disabled reboot trial then found a concrete defect:
Thread correctly remained disabled, with Matter control reachable over Wi-Fi,
but the native camera reset both mute flags from 1/1 to 0/0. Speaker volume
remained 80. The trial restored both controls and removed only its own temporary
fabric, retaining the original three. A native setter/readback does not make
the mute preference durable.

The 0.5.3 source candidate saves an explicit global audio request in the app's
existing KVS before changing native mutes. Both the standard Mode Select command
and the legacy vendor global AudioEnabled request use this path. Startup loads
only a present valid preference and queues its native reapplication before the
vendor worker starts; native readback remains the state source. Installations
without an explicit preference retain the previous startup behavior.

Own Matter microphone/talkback delivery stays gated while mute readback is
unknown. This does not establish acoustic silence, or absence of transient
native audio during the earlier vendor startup interval. The user deferred the
physical listening/stimulus test; that test remains open.

All 179 host tests passed except one optional codec test skipped. The pinned
SDK rebuilt successfully with metadata 0.5.3. Device installation and repeated
post-fix reboot/stream observations are still pending.
