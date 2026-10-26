# Audio and Thread controls — installed candidate

The 0.5.1 candidate implements audio and Thread controls through the camera's
existing Matter pairing. Both control paths passed guarded device off/on trials
on 2026-10-26, retaining the three original fabrics and the running native camera
and media broker. This establishes command/readback behavior, not acoustic
privacy, reboot persistence or a stable-release qualification.

The firmware also exposes standard **Mode Select** servers (cluster `0x0050`):
endpoint 1 is **Camera audio**, endpoint 0 is **Thread radio**. Both advertise
Disabled (0) and Enabled (1), with revision 2 and no standard semantic namespace.
`ChangeToMode` queues the same existing workers; `CurrentMode` reports only the
observed state and fails while unknown/pending. No plug/light device type or
whole-camera power claim is introduced. HA's core Matter integration discovered
these two labelled mode selectors automatically, without another pairing.

The companion switches invoke this standard command and verify the vendor
observations below. This matters for Matter(.js) Server 1.4.0: its
[write handler](https://github.com/matter-js/matterjs-server/blob/v1.4.0/packages/ws-controller/src/controller/ControllerCommandHandler.ts#L947)
rejects attributes absent from its cluster model, although generic reads work.
The earlier vendor-only 0.5.0 write path therefore failed in HA without changing
the native mute settings. No custom Matter-server build is required by 0.5.1.

## Geluid (audio)

On enables both native microphone and speaker; off mutes both. Speaker volume
is retained. Before either change, the Matter app stops its microphone delivery
and invalidates pending talkback. Its worker then changes native `audio in_mute`
and `audio out_mute` and reads both settings back. Only two matching observed
values produce a known switch state; pending, mismatched or failed observations
remain unknown. Startup reads the native configuration without changing it.
The native vendor saves those mute settings through its usual configuration
path; reboot persistence still requires a device trial.

The standard per-stream Matter microphone/speaker mute and livestream privacy
remain additional restrictions. Enabling global audio does not remove a
separately selected Matter stream mute/privacy restriction.

Development image cluster `0xFFF1FC11`, endpoint 1, revision 2, attribute 3:
nullable Boolean AudioEnabled, writable Boolean request. A successful write
accepts an asynchronous operation; it is not physical audio-effect proof.

Native settings readback is not an acoustic guarantee: this camera's native
setter can acknowledge a request despite a driver failure. Before calling this
a qualified privacy control, test native decoded microphone audio against an
audible stimulus and speaker silence, with a local guard restoring exact prior
mute values. Do not close/reopen the broker's native RTSP sessions for that test.

## Thread

On issues `ifconfig up` then `thread start`; off issues `thread stop` then
`ifconfig down`, on the existing serialized UART worker. The dataset is retained;
no radio flash, reset, network re-commissioning or GPIO write is involved.
Actual state requires correlated CLI replies and `state`/`ifconfig` verification.
A successful Matter write queues the change; pending/error is not success.

The app saves an explicit requested preference in its existing KVS before
queuing a command, and reapplies only that explicit preference at startup.
An installation with no preference performs no startup write. Persistence and
reboot behavior have not been hardware-tested for this candidate. The camera's
Matter control uses Wi-Fi, so this design keeps control reachable with Thread
disabled; that separation still needs a device verification.

See [THREAD.md](THREAD.md) for attributes 24–27 on endpoint 0/cluster
`0xFFF1FC10` revision 3. These append to existing telemetry without renumbering it.

## Home Assistant

The 0.5.1 extension adds **Geluid** and **Thread** switches to its existing
linked camera device, using the same Matter node and unique identity. It sends
writes over the existing Matter client and waits for observed confirmation.
Unavailable/unknown values are never displayed as a verified off state. After
a command failure, a later fresh radio observation restores its actual switch
state and keeps retry available; the requested state and bounded last error are
shown separately. Remote
operations are retained across local waits to prevent queues of abandoned
requests. Older firmware without these optional attributes creates no switches.

## Full video off — unresolved

The requested third control must also stop fresh RTSP video and snapshots.
It is **not implemented or advertised** by this candidate. The existing Matter
livestream privacy setting only suppresses this app's output. The native
`video on 0` setting does not stop H.264/H.265/JPEG publication, so it cannot
serve as this control. No native process was paused, killed or patched.

Static analysis found native VI/VENC procedures but did not establish a safe
external, reversible on/off API. Pausing the native process is also unqualified:
a partial native RTSP record can exceed the broker's two-second read deadline,
and disconnecting clients could exercise the known native cleanup problem.
Source gating, all snapshot caches, off/on recovery and the broker lifecycle
must be qualified together before offering a full-image-off switch.
See [VENDOR-MEDIA.md](engineering/VENDOR-MEDIA.md).

## Evidence and outstanding hardware work

- Full host suite: 151 tests run, 150 passed, one optional codec test skipped.
- Real local socket peer tests cover dishonest native setting acknowledgements,
  pending/failed readbacks and unchanged volume.
- Pseudo-terminal tests cover Thread stop/down and up/start ordering, readback,
  UART contention, queued replacement and no default startup mutation.
- Both worker tests also passed AddressSanitizer/UndefinedBehaviorSanitizer.
- Candidate ARM app and pinned SDK build succeeded with firmware metadata
  `0.5.1-controls-candidate`; the exact image passed the guarded device trial.
- HA fixtures verify existing identity reuse, scoped writes, missing capabilities,
  initial unknown preference, null/false distinction, pending/error and cleanup.

The native mute flags changed from 0/0 to 1/1 and back to 0/0; speaker volume
remained 80. Thread was verified disabled, the camera's Matter control remained
reachable over Wi-Fi, and it returned to router after enabling again. The
temporary test fabric was removed, leaving the original three. A detached local
restore guard was proven before mutation. Neither native camera nor broker was
restarted. No dataset/key read or write was used for this trial.

Physical microphone/speaker silence, native setting persistence and explicit
Thread preference persistence across a normal full boot remain unqualified.
The full-image-off control and the earlier long-duration media issue remain
open. Do not promote this candidate to a stable privacy release.

## Exact offline image

- App: 8,307,952 bytes, SHA-256
  `8b8446afa8596a93f8d4477677ce2b27f9076fed1397a626ec39b0275a02c245`.
- Broker is unchanged: SHA-256
  `5c1940064665258cb695f81e1279287316aece034331fed2e0930c8cca8e4399`.
- These hashes identify the installed source candidate, not a stable release.
  The historical public prebuilt is unchanged.
