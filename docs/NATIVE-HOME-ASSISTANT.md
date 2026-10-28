# One native Home Assistant Matter device

Use Home Assistant's built-in Matter integration and its Matter Server. No
project add-on, custom integration, MQTT connection, second setup code or RTSP
configuration is required for the standard controls described here.

## Scope

The qualified camera runs the 0.5.4 native-diagnostics candidate. The historical public prebuilt is
not replaced by these instructions; qualify the actual installed build first.

| Function in HA 2026.10.4 | Native-only behavior |
| --- | --- |
| Device identity and pairing | One existing Matter node/device page |
| Global sound | Standard Mode Select: Camera audio, Disabled/Enabled |
| Full video capture | Standard Mode Select: Camera video (restart), Disabled/Enabled |
| Thread radio | Standard Mode Select: Thread radio, Disabled/Enabled |
| Live view, snapshots, microphone playback, talkback | No native camera entity in this tested HA version |
| Motion | Native binary sensor; held image motion, not stationary presence |
| Thread diagnostics | Native channel, network name and routing role sensors |
| IP and RTSP path | Existing Matter info plus optional device label for `/14` |
| Thread mesh routing | Continues independently of HA entities |

The three controls are **select entities**, not on/off switch entities. They use
standard Mode Select cluster 0x0050 on endpoints 1 (sound), 2 (video), and 0
(Thread). Do not advertise a fictitious plug, light or second camera simply to
obtain different UI widgets. The radio is a Thread mesh router, not an IP border
router. Matter control reaches the G2H over Wi-Fi even with Thread disabled.

Sound Off mutes both microphone and speaker. Full video Off suppresses capture,
recording and RTSP at boot; both Off and On require a full camera reboot. It is
not an instant shutter. See [CONTROLS.md](CONTROLS.md) for the measured guarantees
and remaining acoustic/privacy qualification limits. Saved recordings are not
deleted.

See [NATIVE-DIAGNOSTICS.md](NATIVE-DIAGNOSTICS.md) for the four native entities,
their enable/display steps, partial compatibility model and motion-test limits.

## Install or migrate

For a new native-only installation, pair once through HA's normal Add Matter
device flow. Do not install the project custom component or add its YAML key.
Retain the camera's Matter camera clusters for compatible controllers; absence
of an HA camera entity does not mean the firmware has removed those clusters.

For an existing installation using the optional project extension:

1. Back up that extension's files and config entry, the HA configuration, and
   its relevant device/entity registry records. Keep the existing Matter pairing.
2. Disable only the **G2H Matter Camera** project integration. Verify the three
   native controls on the **Matter** device page before deleting anything.
3. Remove the top-level `g2h_matter_camera:` key from `configuration.yaml`.
   Validate the HA configuration and restart HA, so the observer cannot recreate
   the extension entry.
4. Remove only the project integration entry after confirming the displayed
   deletion scope. Never delete the camera from the Matter integration: that
   would remove its pairing. This removes the extra linked device page/entities.
5. Archive the `custom_components/g2h_matter_camera` folder outside HA's
   `custom_components` directory. Keep the optional extension's source and a
   private recovery copy if it might be needed later.

No firmware replacement, radio flash, factory reset, dataset change or new
pairing is required for this migration when the installed firmware already
exposes the three standard clusters.

## Unsupported-camera indication

HA 2026.10.4's Matter discovery mapping has no camera platform. Unsupported
camera functionality is omitted; the firmware cannot force HA to create a
disabled camera entity or an unsupported-feature banner. An optional device
note or label can explain the missing live view using HA's normal device-edit
UI, without installing any project component. This is an HA-side annotation,
not a firmware-advertised capability or an automatically generated warning.
The qualified installation uses a normal HA device label to mark the missing
Matter live-view support; a new installation must add that label itself.

## Status after a video reboot

The current HA/Matter Server combination can retain the pre-reboot mode in its
cache even when the physical operation has completed. During the native-only
acceptance trial, a manual **Re-interview device** refreshed the correct Off
value. On the subsequent On boot, the HA selection eventually refreshed
automatically; that does not establish a reliable refresh delay. Do not interpret a cached Enabled/Disabled selection as independent
proof of physical capture state. If it stays stale after the camera returns,
use the Matter device's menu, Re-interview device, Start re-interview. This uses
the existing pairing. The reason for the delayed subscription/cache refresh
has not been isolated by this migration; no automatic-refresh fix is claimed.

## Native-only migration qualification on 2026-10-28 (0.5.3)

With the project entry disabled and the automatic YAML loader removed, HA
configuration validation and an HA restart succeeded. Native HA selections
issued the following actual commands through the existing Matter pairing:

- Sound Disabled: native microphone/speaker mute flags both became 1, with
  speaker volume unchanged at 80. Enabled restored both flags to 0.
- Thread Disabled: fresh radio state became disabled while the Wi-Fi Matter
  control path remained available. Enabled reattached; router role was observed
  again after the subsequent camera boot.
- Video Disabled: after a full boot, ISP frame count stayed zero in two samples,
  VI/VENC channels 0 and 1 were disabled/uncreated/stopped, and the RTSP listener
  was absent. Fresh Thread router state and all three original fabrics remained.
- Video Enabled: another full boot restored ISP frame progress, channel 0
  capture/encoding and the RTSP listener; Thread returned to router, with the
  same three fabrics and the persistent network hostname.

No listening test or test sound was performed. No firmware binary was changed
for this acceptance trial. HA's native display still requires the cache-refresh
qualification described above, and a newly commissioned second physical camera
was not tested. The existing device's pairings and native entity IDs are retained.

The final migration removed the project config entry, its linked device page
and all 14 project entities after user confirmation. Project files were archived
outside `custom_components`; the automatic YAML loader is absent. The original
Matter device and its three enabled native select entities remained. HA's native
device label explains that Matter live view is not yet supported. No companion
code remains on the active integration path, and no camera re-pairing was used.
The installed HA version was verified from `.HA_VERSION` as 2026.10.4; the earlier
2026.10.2 media/extension evidence in this repository remains historical.
