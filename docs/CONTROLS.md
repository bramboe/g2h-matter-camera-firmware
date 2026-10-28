# Audio, video and Thread controls — 0.5.3 candidate

All three controls use the camera's existing Matter node and pairing.
They use standard **Mode Select** (cluster `0x0050`), revision 2:

| Control | Endpoint | Disabled (0) | Enabled (1) |
| --- | --- | --- | --- |
| Geluid | 1 | Microphone and speaker muted | Microphone and speaker enabled |
| Video | 2, optional utility | Capture/recording/RTSP tasks suppressed at boot | Normal capture startup |
| Thread | 0 | Thread stopped and interface down | Interface up and Thread started |

A command accepts a queued operation. Only independently observed state is
reported as CurrentMode. Pending, failed or unknown observations are never a
verified off state. The new optional video endpoint uses a vendor development
utility device type (`0xFFF10001`); it does not pretend to be a plug, light or
second camera. Existing camera and radio endpoints are unchanged.

The standard transport also works with controllers that reject writes to an
unknown vendor cluster. The companion Home Assistant extension adds labelled
switches using the same node and identity, without another pairing or a custom
Matter server. Existing linked Matter/companion device pages are retained.

## Geluid

Off mutes both native `audio in_mute` and `audio out_mute`. On enables both;
speaker volume is retained. Before changing them, the Matter app stops its
microphone delivery and invalidates queued talkback. Its worker applies each
native setting and reads both back. Both values must match before the state
is known. Startup reads existing native settings without changing them unless an explicit global preference
was saved by this app. The native mute flags themselves reset at boot; the
0.5.3 app saves and reapplies that explicit preference from its existing KVS.
Both standard Mode Select and the vendor global AudioEnabled write use it.
Own audio delivery stays closed until native readback. Early native startup
transients before this service runs have not been acoustically qualified.

Per-stream Matter microphone/speaker mute and livestream privacy remain
additional restrictions. Enabling global audio does not remove those settings.
Native readback is not physical acoustic-silence proof: the vendor setter can
acknowledge a write despite a driver failure. The guarded 2026-10-26 trial
verified native mute flags 0/0 → 1/1 → 0/0 with volume unchanged at 80; a complete
acoustic privacy qualification remains separate.

The development image cluster `0xFFF1FC11`, endpoint 1, attribute 3 retains
nullable Boolean AudioEnabled for observation and asynchronous requests.

## Video

This optional backend requires an exact verified native-image/helper profile.
It is not enabled merely by installing the new app. The [reproducible helper build](VIDEO-BOOT-BUILD.md) uses private native inputs.
The installed wrapper checks that profile before loading the boot gate. An invalid explicit off
configuration fails closed rather than launching normal capture.

**Both off and on require a full camera reboot.** Wi-Fi, Matter and Thread are
briefly interrupted during that boot; the existing fabrics and Thread dataset
are retained. The app first closes its own video/snapshot gates, erases cached
frames and ends the Matter media session. It then atomically persists the
requested video preference and queues the reboot. This is not an instant
hardware shutter and it is not a timer-based privacy mode.

With video off, a small exact-profile preload blocks native task startup for
video, recording, RTSP and short/cloud recording. It edits the process's task
start dispatch in RAM; it does not rewrite the vendor executable. The explicit
preference persists across subsequent boots. Image-setting writes through our
app (nightvision, WDR, brightness and OSD) are blocked while capture is not
verified on. Audio, status LED and Thread controls remain separate.

The reported off state requires the explicit off preference, private pause
marker, zero ISP frame count and uncreated/stopped VENC plus disabled VI
channels. On requires normal preference, advancing ISP frames and active
channel 0. Queuing a reboot never reports the desired state optimistically.
Existing saved recordings are not deleted by this switch.

The installation trial has independent timed and boot-count recovery. After a
successful commit those automatic restoration actions are disarmed/removed,
so a user's later video-off selection cannot silently time out back to on.
Keep the private startup backup for an explicit terminal recovery.

Both standard Matter off/on commands passed the bounded device trial. OFF
had zero ISP/VI/VENC activity, no recording task/descriptor and no RTSP listener;
ON restored decoded Matter video/audio. Original fabrics and fresh Thread
router status were retained. See the [test report](TEST-REPORT-2026-10-28.md).
The historical public prebuilt is unchanged; this is a candidate rather than
a long-duration stable privacy release.

## Thread

Off issues `thread stop`, then `ifconfig down`, through the existing serialized
UART worker. On issues `ifconfig up`, then `thread start`. No dataset erase,
radio flash, GPIO reset or re-commissioning occurs. CLI replies and subsequent
`state`/`ifconfig` observations establish actual state.

An explicit requested preference is saved in the app KVS before queuing the
command and is reapplied at startup. With no preference, startup does not
change the radio. The Matter app is controlled over Wi-Fi, so it remains
reachable with Thread disabled. The guarded device trial verified off and
return to router, retaining the original three fabrics. Explicit disabled
preference persistence still needs separate reboot qualification.

See [THREAD.md](THREAD.md) for unchanged observations 24–27 on endpoint 0,
cluster `0xFFF1FC10`, revision 3. This radio is a Thread mesh router; it is not
an IP border router.

## Home Assistant behavior

Version 0.5.2 of the companion extension probes video capability separately.
Older firmware continues to offer its supported audio/Thread controls; it does
not acquire an ineffective video switch. After a video command the switch is
unknown/unavailable until a later fresh physical observation after the reboot.
The audio and Thread switches wait for their observed readbacks. A later fresh
observation permits retry after a failed command.

One retained, bounded Matter operation prevents abandoned RPCs from building
up. A timed-out optional video request temporarily marks controls unavailable
until it finishes. Ordinary unsupported-attribute errors do not hide supported
controls. These are UI status semantics, not a substitute for device testing.

## Verification

175 host tests ran: 174 passed, one optional codec test skipped. Actual local
native-setting and UART peers, dishonest acknowledgements, privacy cache races,
boot-service failure paths, unchanged identities and bounded RPCs are covered.
The new app passed a 60-second guarded device qualification, preserving native
process and host boot, three fabrics, fresh attached Thread and healthy media.
See [TEST-REPORT-2026-10-28.md](TEST-REPORT-2026-10-28.md) for exact hashes and
subsequent device evidence.

Runtime native sensor stop/start is retained only as research source. It can
restore ISP frame counts while leaving the established media pipeline broken;
it is not used by this video control. Source-only gates or native `video on 0`
are likewise insufficient to establish full capture-off.
