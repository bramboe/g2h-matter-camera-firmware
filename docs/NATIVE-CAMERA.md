# Native Matter camera in Home Assistant 2026.11 beta

On 2026-10-28, Home Assistant Core **2026.11.0b0**, frontend **20261028.0**,
created a native camera entity on the existing G2H Matter device. Live playback
was verified through that entity, without the project companion integration,
an additional pairing, an RTSP integration or a second device page.

The running camera app was **0.5.5-native-settings-candidate**, SHA-256
`3facc339feb26b84b748911f2874918531507efc346e33ec41c71c18905fe642`.
The pending 0.5.6 language app had rolled back after its guarded qualification
timed out; the native-camera result is explicitly a result on 0.5.5.

## Use

Open the G2H's existing Matter device and select its camera entity. The firmware
still exposes the same standard Camera AV Stream Management and WebRTC
Transport Provider clusters on endpoint 1. HA's new camera platform uses them
through the existing Matter Server API (schema 14), as described in the
[official camera-support change](https://github.com/home-assistant/core/pull/176080).

Close other Matter live views of this camera first: this build admits **one
WebRTC session** at a time. RTSP and the Thread mesh-router function remain
separate. Wait for negotiation to finish; the initial test displayed “Unable
to play media” before starting successfully. Persistent errors should still be
investigated rather than attributed to that initial display automatically.
No general reconnect fix or identified root cause for the initial failure is
claimed: a fresh connection worked after the guarded app rollback/restart.

The device's Idle state means no active Matter video session. It does not mean
the video/privacy control is Off. Full capture Off still follows the existing
reboot-based behavior documented in [CONTROLS.md](CONTROLS.md).

## Evidence and limits

The rendered browser video reported 1280×720, readyState 4, paused false and
no media error. Its playback clock advanced from 25.27776 to 98.239875 seconds.
Independent read-only camera observations recorded connected sessions,
increasing video/audio packet counters, three original fabrics, a fresh Thread
router and no source/audio faults. The owner then independently confirmed
moving video in HA. After the agent closed its viewer, the camera reported
ended-by-peer and no active session. No household video or still image was
downloaded, recorded or included in a source repository.

These observations qualify live video for this exact beta and app combination.
They do not establish a new native HA talkback, recording, speaker media-player
or snapshot-card result. The prior snapshot trial through the standard Matter
Server UI remains documented in [NATIVE-SETTINGS.md](NATIVE-SETTINGS.md).

Earlier documentation about HA 2026.10.4 lacking a native camera platform is
historical. On the qualified upgraded installation the old unsupported-live-view
label was detached from this device and its note was updated. Device identity,
existing controls, fabrics and radio configuration were retained.

## Final 0.5.6 regression check

After the language candidate was successfully installed and its Dutch selection
survived an own-app restart, native HA live video was checked again. The browser
reported 1280×720, readyState 4, paused false and no media error, with its playback
clock advancing from 54.804702 to 82.513063 seconds. Independent camera counters
also increased; RTSP DESCRIBE returned 200 and Thread remained a fresh router.
The viewer was then closed to release the single session. This establishes the
same native-camera result on the final qualified 0.5.6 app, without saving media.
