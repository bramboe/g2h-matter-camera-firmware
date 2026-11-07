# Video-control preparation and existing-source recovery — 2026-10-27

The installed Matter app remains 0.5.1-controls-candidate; the research app
build is not installed. Later one-shot native-helper trials used full host
boots and restored the original startup configuration. A subsequent subsecond native sensor OFF/ON trial passed the startup
preconditions and frame-counter verification; see the trial notes below.

## Source preparation

- Separate global gates clear own video/IDR/snapshot buffers; old-epoch reads,
  fragmented access units and late JPEG results cannot refill those caches.
- The subscriber reader excludes explicit capture-pause time from its normal
  record budget, even across a partial header. It remains cancellable within
  bounded polls. Normal unpaused timeouts and corrupt records still fail.
- The delegate rejects new stream allocations while globally blocked.
- A separate opt-in native-control prototype has no startup driver call; its
  fake-driver tests cover hung-call responsiveness, superseded requests and
  refusal to call an unknown result a hardware state. It is not in the build.

The full host suite ran 154 tests:153 passed, one optional codec test skipped.
New source/snapshot/native-worker tests use AddressSanitizer and
UndefinedBehaviorSanitizer. The normal ARM app build succeeded. Neither
host tests nor a successful build qualify a complete video-off control.

## Existing installed source

An idle source fault was observed on the installed 0.5.1 app while the retained
broker and native camera remained alive. A bounded broker subscriber received
137 records/169520 payload bytes in two seconds. A subsequent Matter video
request recovered using the existing reopen path, without restarting anything:

- 120 H.264 frames received and decoded; CASE and DTLS/SRTP connected.
- 307 Opus frames received; zero audio decode errors.
- No talkback audio transmitted and no household media saved.
- The temporary test fabric was removed, leaving the original three.
- Host boot/native process/broker lifetime were unchanged; Thread remained router.

This proves the bounded recovery path, not a fix for the earlier intermittent
long-duration media problem.

## Remaining qualification

The native source must still be stopped and restored, including SD recordings,
without treating a black overlay, delivery restriction or a driver-shaped
acknowledgement as capture-off. Native and hardware buffers, startup behavior,
old-frame disposal and long-duration recovery remain unresolved. There is no
new video-off entity in HA from these changes.


## Native-helper compatibility and startup trials

An isolated helper process successfully loaded the research shared library
against the exact installed ARM glibc 2.19 runtime and served root-only status
requests without calling sensor functions. Modern direct stat imports were
replaced by the matching old ABI, ordinary and PLT relocation tables were
made contiguous for the old loader, and runtime dependencies are explicit.
The helper exports only its intended cameraPreview interposition symbol.
No vendor library is included in this repository.

The first native startup trial used a process path longer than the monitor's
64-byte storage and did not start the native camera. The original XML was
restored and a full boot recovered native capture, Matter's three existing
fabrics and Thread router. The revised trial uses a shorter process path and
an independently started boot guardian that restores startup even if the
native wrapper never runs. Both armed and disarmed recovery paths were
executed locally against temporary files.

The revised native startup reached camera/helper/Matter readiness with no
sensor call, and original XML was verified restored before a sensor request.
Its short test window ended while Thread was still attaching, so it returned
to the baseline without issuing OFF/ON. Sensor-stop qualification remains open.


## Short native sensor pulse succeeded

The final bounded trial established actual counter cessation and recovery:
ISP FrameNumber advanced 701→704 before OFF, remained 704→704 during the
short OFF interval, then advanced 708→712 after native power-on and init.
The independent local ON guard completed successfully. OFF/ON returned zero,
but the separate counter observations are the evidence of capture effect.
This was a subsecond experiment, not a long-duration privacy qualification.

Original startup XML was restored and the normal camera software was loaded
again by a full boot. Matter retained its three existing fabrics, source status
was healthy and Thread was freshly observed as router. No raw flash, dataset,
key or household media recording was read or saved by this test.
SD recording cessation, all output paths and long OFF/ON recovery remain open.


## Candidate long-pause preparation

The candidate app now follows the research helper's local pause marker and
clears its source/snapshot/delegate copies before resuming after a short drain
barrier. The candidate broker suspends intentional partial-record pause time,
discards video records that cross a pause and retains established native
sessions. New native sessions are refused while paused. Existing audio remains
independent. Invalid marker state suppresses video while retaining native ingest.

Local tests cover a partial header paused longer than the normal two-second
record deadline, fresh whole-record recovery, refusing native initialization
while paused and preserving an existing audio subscriber. The ARM app and
broker build succeeded; neither is yet installed as a release.

An opt-in recorder sequence remembers whether the native record task ran,
stops it before sensor power-off and restores it after sensor initialization.
Host tests cover failure at every transition and preserving a previously stopped
recorder. The extended shared helper passed isolated loading/status checks on
the exact native glibc runtime without sensor/recorder requests. Only its intended
symbol is exported; imports resolve in the retained native runtime libraries.
The live long-off/record-thread trial remains to be reported separately.


## Longer sensor and recorder-thread trial succeeded

A separate guarded native trial enabled the explicit recorder-sequencing option.
ISP FrameNumber advanced 1287→1290 before OFF, remained 1290→1290 across a
five-second measurement during OFF, then advanced 1294→1298 after restoration.
The named native record_task thread count changed 1→0→1. The independent local
ON guard succeeded, and the native process identity remained unchanged through
the complete stop/restore operation. No native camera/broker warm restart ran.

The original startup XML and normal installed software were restored with a
full boot. The existing three Matter fabrics and fresh Thread router status
were retained. These observations qualify the measured native stop/restore
sequence; SD file-write metadata, a complete deployed media pause, reboot
privacy and the user-facing video switch are still not qualified.

## App-only pause-gate trial and update qualification

The first app-only attempt exhausted its qualification window and rolled back
successfully. The previous installer allowed only 20 seconds for startup before
a continuous 60-second qualification, with no per-sample diagnostics.
The revised installer records only allowlisted status fields and permits a
separate bounded startup allowance. Its deadline remains at least 30 seconds
before the independent 180-second rollback timer; stale status restarts the
continuous observation window. An initially attached Thread node must regain
an attached role rather than qualifying while merely fresh-but-detached.
Synthetic tests cover delayed first observations, status gaps, detached roles
and never-fresh rollback within the guardian budget.

The same app image (`7d5229d7d3f7db94997c87984aec6a84353ad8f77643874499ddc5737b331a28`)
subsequently passed the guarded app-only trial. Measurements captured startup
Thread detached/stale observations followed by fresh router status. Final
status retained three fabrics, ready media broker and no source fault. Native
camera PID/start time and host boot identity did not change. This qualifies the
app-only update with the existing broker; it does not qualify the full video
control or replace the pending combined native/broker trial.

## Combined broker/native stop trial: recovery still fails

The first combined trial loaded the candidate app and broker by a full boot,
with the native helper enabled for this boot only. Before stopping capture,
a temporary CASE controller decoded 120 H.264 frames and received 441 Opus
packets. The sensor then stopped (ISP 1414→1414 over five seconds), the native
record task changed 1→0, and independent local restoration returned both
(ISP 1418→1422, record task 0→1). Native lifetime remained unchanged.

The subsequent Matter video allocation failed before Offer/Answer signalling
with general IM failure. This does **not** establish encoder or subscriber
recovery, despite the returning ISP counter. The temporary fabric was removed,
original broker/startup restored, and a normal full boot confirmed three
original fabrics, fresh Thread router status and no source fault. A further
trial adds bounded broker stage/counter diagnostics to locate that failure.
The full-video switch remains unfinished and is not advertised.
