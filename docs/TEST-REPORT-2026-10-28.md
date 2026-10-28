# Video boot control candidate — 2026-10-28

**Latest installed state:** app 0.5.3 (`7eacb7af…`), companion extension 0.5.2.
Video OFF/ON and audio/Thread disabled cold-boot checks passed. Two awake
7,200-frame sessions passed with idle reopening. Physical listening/stimulus
qualification remains deferred by the user. Earlier entries below retain the
failed-trial evidence and explain the final implementation.

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

### Late native initialization reset

The first 0.5.3 implementation saved/reapplied the choice once. The repeated
boot trial still observed native flags 0/0 after the new image was verified
as the running executable. This showed that one startup application was
insufficient; the native initializer can reset an earlier acknowledgement.
Both failed trials restored the original controls and removed their temporary
fabrics. No radio/native image was flashed.

The final source keeps global OFF authoritative: its worker verifies both
native mutes every two seconds and reapplies contradictory or unknown values.
A confirmed ON request releases enforcement, preserving later external mutes.
Own microphone/talkback delivery is additionally blocked directly by the
explicit OFF preference even if the native service resets its state.

A real local peer regression simulates exactly that reset after a successful
acknowledgement, checks remuting, unchanged volume, and release of enforcement
after ON. It passes. The full suite again passed 179 tests with one optional
codec skip. One sanitizer cancellation test hit its former 300 ms combined
assertion under concurrent compilation; its isolated rerun passed. The host
test now separately checks source faults and allows one second for scheduler
and sanitizer teardown, still detecting an unbounded paused reader. Production
video cancellation code was not changed by this test adjustment.

Final candidate app SHA-256:
`7eacb7af6f93b701aa1b3f306be0628c536f8983acdca8956b428c5dd23e4ab2`.
Its device qualification follows below once complete.

### Corrected cold-boot persistence passed

The final 0.5.3 image above passed guarded installation with unchanged native
process/host lifetime and the original three fabrics. A subsequent real full
boot with both controls OFF then confirmed: native microphone mute=1, speaker
mute=1, unchanged volume=80, fresh Thread disabled, and reachable Matter over
Wi-Fi. Standard Mode Select state reads succeeded. The trial then restored
both controls to ON. This is native-setting persistence evidence; the separate
physical listening/stimulus test remains deferred by the user.

The two longer post-reboot media/idle-reopen phases completed; final results
and the separately identified Mac sleep interruption are recorded below.

### Interrupted post-boot duration: Mac low-battery sleep

The first corrected-image duration attempt decoded 5,310 frames and received
13,105 Opus packets before its peer failed. The concurrent Telnet socket then
reported macOS errno 49 (source address unavailable), and the test could not
perform its planned cleanup over that socket. This was recorded as a failed
trial, not a passing soak.

The Mac power log establishes Low Power Sleep at 09:04:21 local time with
battery=1%, followed by wake at 09:13:55 on power attachment/user activity.
After reconnecting, the camera's boot/native process identity was unchanged.
Only the leftover temporary fabric was removed, returning the original three;
video remained on and Thread was router. The Mac subsequently reported AC
power and charging. This explains this specific interrupted observation, not
the historical failures for which equivalent power evidence is unavailable.

The two stream/idle-reopen phases were repeated successfully under a test-scoped
idle sleep assertion. No permanent Mac power setting was changed.

### Final awake duration and idle-reopen results

The final installed 0.5.3 candidate completed both requested 7,200-frame
sessions, separated by a 45-second idle interval and normal reopening:

| Phase | Elapsed | Actual decoded frame checksums | Received Opus packets | Result |
| --- | --- | --- | --- | --- |
| First continuous session | 366.4 s | 7,200 | 17,917 | Pass |
| Reopen after idle | 367.1 s | 7,200 | 17,952 | Pass |

Both CASE/ICE/DTLS/SRTP paths connected and FFmpeg decoded every requested
frame with exit zero. No empty frame drops, native/host restart or status-read
error occurred. The original three fabrics were restored after removing only
the temporary controller; final status was healthy, Thread router and no active
video session. No household video or audio file was saved.

These observations cover more than twelve minutes of active receiving plus
idle reopening on this network path. They do not prove broad long-term
stability, identify the old 25 October failures or qualify physical acoustic
silence. No matching Mac sleep transition was found around the earlier
25 October 16:30–17:00 failure window, so today's low-battery explanation is
not retroactively applied to that historical failure.

### Muted microphone transport check passed

With global audio OFF, a separate authenticated receiver completed encrypted
signalling and decoded 120 video frames while receiving zero microphone Opus
packets. The app's audio-sent counter delta was zero while its video-sent delta
was 321. No test sound or talkback was sent. The receiver normally expects audio,
so its non-success result while muted is intentional; the independently checked
video, zero-audio and app-counter observations are this negative test's result.

Global audio ON was restored. A subsequent ordinary 120-frame video/audio
request passed, the host/native identity was unchanged, and the temporary
fabric was removed, leaving the original three. This verifies the Matter
microphone barrier, not physical acoustic silence or native RTSP audio samples.
