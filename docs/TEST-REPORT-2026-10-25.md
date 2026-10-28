# Motion and media runtime follow-up — 2026-10-25

The source candidate below is installed and has passed the checks listed here.
It remains a development candidate, not a fully qualified stable release.
Historical images and the user-confirmed clear HA microphone listening check are
recorded in [the earlier report](TEST-REPORT-2026-10-24.md).
No device address, setup code, fabric credential, Thread dataset, native process
memory, captured camera image or recorded microphone audio is included here.

## Exact current images

- App: `0.4.0-candidate`, 8,296,752 bytes.
- App SHA-256: `5612491306a17a30a622e03b1624238fb8d53015c2a3590432795461d7044526`.
- Broker: 213,164 bytes.
- Broker SHA-256: `5c1940064665258cb695f81e1279287316aece034331fed2e0930c8cca8e4399`.
- The historical prebuilt in `app/` remains identified separately. It has not
  been silently replaced by these candidate binaries.

## Motion findings and resulting behavior

The earlier two-second app recorded 47 reads, one local expiry during an
in-flight read, no native stale samples and no read errors. The slowest response
took 1,190 ms. This established a controller expiry during transport delay in
that interval; it did not prove that all later outages had the same cause.

The current app advertises a four-second freshness lease: one-second producer
cadence, a two-second read deadline and scheduling margin. HA counts the full
request/response duration against that lease. A slow response never extends it.
A transient timeout or OS transport error can retain an existing valid sample
without changing its timestamp or expiry, followed by one prompt 50-ms retry.
Normal reads remain spaced by 500 ms and never queue behind an in-flight read.
Malformed/unsupported data still fails closed. Disabled, failed and expired
samples are unavailable, never a fabricated clear state. Changing sample age
alone does not generate an entity update or dirty Matter attribute notification.

A four-second lease alone still produced short availability changes on read
timeouts. The final transient-error handling addresses that separate case.
After its installation, a 464-second diagnostic interval recorded:

| Observation | Result |
|---|---|
| Motion reads | 750 additional reads |
| Native stale samples | 0 |
| Read errors | 18 |
| Errors retained within the original valid lease | 2 |
| Actual lease expiries | 1; sample age reached 4,002 ms |
| Recovery | Fresh motion data recovered; detected-to-clear transitions observed |

This is a reduction of premature state changes, not a claim that the control
path never stalls. One longer control-path interruption correctly became
unavailable. Timing counters distinguish it from repeated unchanged-age updates.

The subsequent HA refinement retains one underlying remote RPC across local
two-second wait timeouts, with a thirty-second remote bound. This prevents
local timeout retries from accumulating server operations. A late response is
still aged from its original start; the four-second availability lease is
unchanged. Regression tests cover shared remote reads and unload cancellation.
The earlier hardware measurements above precede this refinement; they must not
be presented as hardware qualification of the refined coordinator.

## Media startup and recovery

The first media attempt after the app switch failed with a source fault. Before
recovery the broker had no established native RTSP connection. A controlled full
camera reboot restored video while preserving the selected app, native camera
image, three fabrics and fresh Thread-router state. No detailed native failure
phase log was retained, so its exact cause is not conclusively established.

Code inspection found that a failed first TCP connection also latched the broker
permanently. The corrected broker permits an attempt after five seconds only
when its socket constructor failed before sending any RTSP command. A failure
after DESCRIBE/SETUP/PLAY or during ingest remains latched; native sessions are
not reopened. Synthetic socket tests verify late server readiness, bounded retry,
and refusing retries after a handshake failure.

The corrected broker was installed with a verified private backup and retained
previous binary, followed by a full camera reboot. Its exact hash, unchanged
app selector/native image, three fabrics, fresh Thread state and stable ready
window were verified. The broker was never warm-stopped independently.

## Completed checks

- At the earlier checkpoint, 119 host tests passed; one optional real-codec test
  skipped. The current source suite has 140 passing tests and one optional skip.
- Sanitized native observer regression passed, including the exact freshness
  boundary, executable/hash/permission guards and restart handling.
- Both ARM observer/reporting tests linked with the production SDK and assertions
  enabled passed on the camera with unchanged native/host lifetime.
- Guarded app-slot installation passed with a fresh verified backup, unchanged
  native/host lifetime, retained earlier app and all three fabrics preserved.
- Full boot verified the current app image, unchanged native image and Thread.
- The final broker and earlier HA transient-error handling were sampled 31 times over 300.2 seconds:
  host/native lifetime unchanged, all three fabrics retained, no source fault,
  fresh Thread router, zero AAC decode errors, 4,614 video frames and 11,643 Opus
  packets sent. Browser decoding advanced past 388 seconds at 1280×720.
- A final 640×360 HA snapshot decoded independently with FFmpeg while live video
  continued. One preceding snapshot hit the bounded timeout and returned an
  error; its retry succeeded. Snapshot delivery is not claimed failure-free.
- Source/relink bundles and manifest validated; relinking extracted objects and
  libraries from the final bundle reproduced the exact application SHA-256.

An earlier test pause was confirmed by the user as the camera being switched
off. It is not evidence of a firmware crash. The later controlled reboots are
recorded separately from unchanged-lifetime media samples.

## Reverse-audio protocol probe

A bounded synthetic two-second signal was sent through CASE-signalled WebRTC
on a temporary test fabric, with no microphone recording saved. The receiver
reported 100/100 Opus packets sent, and the device counted 100 received packets
and three published/played audio buffers. Independent H.264 decoding passed
for 120 frames; 654 microphone Opus frames were received. No AAC decode errors
were added; host/native lifetime and fresh Thread-router state were preserved.
These counters prove transport and invocation, not physical audibility. The user reported hearing no tones, so this probe did **not** qualify audible
reverse audio. Subsequent read-only native settings showed speaker mute 0 and
volume 80/100; no setting was changed. A separate mute qualification remains
pending. The legacy `talk_played` counter counts completed debug-command
dispatches; the vendor callback can return success despite internal playback
failure. It must not be interpreted as physical output confirmation. This
controller probe also does not establish a talkback button in HA’s camera UI.

The latest HA coordinator was activated and its initial 151 reads recorded no
errors or expiries. Status interruptions then occurred during the temporary commissioning/audio
probe; those are not a clean steady-state motion sample. A later
read-only runtime sampler ended after nine samples (80.4 seconds) when its
Telnet connection timed out. It is incomplete, not an additional passed soak.
The user subsequently confirmed switching off/restarting the camera. On return,
the current broker hash, three fabrics, motion availability and fresh Thread
router were verified; reopening HA decoded 1280×720 video past 128 seconds.

A second bounded CASE/WebRTC probe used the same two-second synthetic signal
with fivefold amplitude (about 0.105 peak, without changing native volume). The
user heard both tones, close together but not loud. Transport again passed
100/100 Opus packets, 120 independently decoded H.264 frames, no added AAC
decode errors and unchanged host/native lifetime. The temporary test fabric
was removed, retaining the original three. This establishes audible reverse
audio for the tested signal, not speech intelligibility, calibrated loudness,
uninterrupted full-duplex operation or a talkback UI in HA.

The post-user-reboot HA diagnostic interval added 541 underlying reads, 14
coalesced waits, 15 local read errors, one error retained within its valid lease,
two genuine sample expiries and no native stale samples. Queue prevention is
active; these observations do not establish uninterrupted control availability.

## Later speech and mute qualification

A synthetic Dutch sentence was encoded as 223 Opus packets, using a normalized
0.35 peak signal and unchanged native speaker volume. All packets reached the
camera; the user confirmed that the complete sentence was clearly intelligible.
Independent decoding produced 240 H.264 frames and 608 microphone Opus frames;
no AAC decode errors were added. The temporary test fabric was removed, keeping
the original three fabrics, unchanged host/native lifetime and fresh Thread.
This qualifies intelligible talkback at that tested signal level. It does not
establish calibrated loudness, uninterrupted full duplex or a HA talkback UI.

A separate temporary standard Matter SpeakerMuted=true write was read back,
then the same 223-packet speech was sent. Camera talk-received, published and
playback-dispatch deltas all remained zero while video and microphone reception
continued. SpeakerMuted=false was restored and verified; the temporary fabric
was removed. This verifies the software delivery/playback barrier, not a
separate acoustic measurement or native microphone capture-off.

## Later stability failures

A completed 604.5-second runtime observation collected 61 samples, including
four Telnet timeouts. Ten successful full identity checks showed unchanged
host/native lifetime, and successful status reads retained three fabrics and a
fresh Thread router. However, the video session ended with peer-failed at about
284 seconds and its video counters stopped growing. Browser currentTime kept
advancing, so it cannot be used as proof that fresh frames continued arriving.
A later snapshot decoded, but was not concurrent with an active video session.
This is a completed runtime observation and a **failed continuous-video soak**.

An independent CASE/WebRTC controller, without the HA browser, also failed its
long-duration target after 1,979 decoded H.264 frames and 4,967 received Opus
frames. Its connection reached ICE/DTLS/SRTP and decoding worked before the peer
failed. Host/native lifetime and the three original fabrics were preserved,
with no added AAC decode errors. This excludes a cause unique to HA's browser;
it does not identify the underlying network or transport cause. No speculative
firmware reconnect patch was deployed.

## HA-only follow-up

The Thread coordinator now retains a single remote read across local waits, so
slow optional tables cannot accumulate more requests. A measured follow-up
recorded three local radio timeouts and two blocked reads. Motion normalizes
already-expired responses and deduplicates actual visible state delivery.
Websocket closure now fails pending requests and releases the local busy video
session, reporting a bounded reopen message. It does not implement transport
reconnection or fix the long-duration failure above.

The 0.4.1 extension adds [automatic attachment](AUTO-ATTACH.md) to the existing
Matter pairing. Tests cover delayed startup, subsequent pairing, live identity
verification, disabled/existing entries, unique-ID duplicates and shutdown.
HA 2026.10.2 presents the extension and Matter as linked device pages, not one
merged page. This source change adds no native HA Core camera support. Configuration
validation and a HA restart passed; the installed observer completed three
initial scans without failures or duplicate entries for the existing camera.
Camera availability, fresh motion and Thread-router diagnostics recovered.

## Remaining qualification

Uninterrupted video/control stability, calibrated talkback level, full duplex,
interrupted updates/physical power-cut recovery, a fresh real-device automatic
attachment, and a clean build on a second host remain unqualified. Global
native capture-off remains unsupported. This device is a Thread router rather
than a Border Router.
