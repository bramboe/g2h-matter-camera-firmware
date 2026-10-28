# Diagnostics and motion on one native Matter device

The **0.5.4-native-diagnostics-candidate** introduced four native entities on
the existing Home Assistant Matter device. No G2H custom integration, MQTT
connection, new pairing, additional device page or radio flash is required.
The three audio/video/Thread Mode Select controls remain unchanged.

The installed 0.5.5 candidate extends this model; see
[NATIVE-SETTINGS.md](NATIVE-SETTINGS.md) for current controls, WiFi diagnostics,
five extra Thread attributes and the existing Matter-server snapshot UI.
The model and qualification below record the earlier 0.5.4 trial.

| Information | Location in HA 2026.10.4 | Firmware source |
| --- | --- | --- |
| IP address | Expand Matter info on the device page | Existing Matter networking information |
| RTSP path | Optional normal device label `RTSP-stream: /14` | Installation annotation; combine with current IP |
| Thread channel | Diagnostic sensor, disabled by default | Observed JN5189 channel |
| Thread network name | Diagnostic sensor, disabled by default | Observed JN5189 network name |
| Thread routing role | Diagnostic sensor, disabled by default | Observed JN5189 role |
| Motion | Native binary sensor, initially named Occupancy | Fresh native camera held-motion signal |

For an existing pairing, run **Re-interview device** from its Matter device
menu. Enable the three Thread entities under Diagnostic. They can be renamed
normally in HA; the qualified installation uses Thread-kanaal, Thread-netwerk
and Thread-rol. Set the binary sensor's name to Beweging and **Show as → Motion**
in its entity settings. This is a normal HA display override, not another
integration. Entity IDs and the original Matter device remain intact.

## IP and RTSP

Use `rtsp://<current-IP>/14` while video is enabled. The IP already appears under
Matter info; it is not duplicated as an invented sensor. The optional RTSP label
is a static HA annotation, not an availability check or a firmware-generated
URL entity. A new installation must add that label itself. There is no native
HA Matter mapping for an arbitrary RTSP URL in this tested version.

Video Disabled suppresses capture, recording and the RTSP listener after the
required full boot. See [CONTROLS.md](CONTROLS.md). Existing recordings are not
deleted. This addition does not restore native Matter live view in HA.

## Read-only compatibility model

The standard Thread Network Diagnostics cluster `0x0035` on existing utility
endpoint **2** exposes only Channel (0), RoutingRole (1), and NetworkName (2),
plus revision 3 and feature map 0. The separate JN5189 is a router-eligible FTD.
Unknown roles and stale data encode null; invalid channels/names are not
invented. It reuses the existing serialized radio snapshots and does not add
UART commands or expose Thread credentials. The root Linux transport remains
Wi-Fi; this does not make the Linux host a Thread Border Router.

Motion uses the standard Occupancy Sensing cluster `0x0406` on existing camera
endpoint **1**, with the Vision feature (`0x80`) and revision 5. Only fresh,
known observations with an enabled producer and permitted capture become a
boolean. Unknown/stale/disabled capture returns an Interaction Model read
failure; it is never encoded as a false clear observation. Controller behavior
after such failures can retain a cached value, so HA's stale/unavailable display
has not been qualified as a privacy or safety indicator.

The legacy physical-modality bitmap is zero and the legacy sensor-type enum is
omitted: the camera has no PIR, ultrasonic or contact sensing modality. This is
a **partial controller compatibility model**, not a claim of full cluster
conformance or Matter certification. Other mandatory diagnostics, topology,
counter attributes and legacy occupancy attributes are not fabricated. Richer
vendor telemetry remains in the firmware for capable clients; it does not
automatically create additional native HA entities.

The binary signal means **held image motion**, not stationary-person presence,
recognition or an alarm guarantee. Its producer settings are not changed by
this feature. Notifications occur on value/availability changes; advancing
sample ages or unchanged samples do not increment the cluster data version.

## Qualification, 2026-10-28

- Source build: app SHA-256
  `352cece3a512f38b2379ad25f460b6621166316b1c5b5d6edd5ec797a534451c`,
  8,327,952 bytes. Historical public prebuilt is unchanged.
- Host suite: 179 tests, 178 passed, one optional codec test skipped.
- `tests/test_native_diagnostics.cpp` was compiled with the production Matter
  SDK and assertions enabled, then executed on the camera. It passed real SDK
  data-version assertions for true/false/unknown transitions, stale radio data,
  role validation and no unchanged-sample notification spam.
- Guarded app-only installation passed 60 seconds of continuous qualification,
  retaining the previous slot, the exact existing media broker, the native
  camera process/host boot and all three original fabrics.
- The original HA Matter page showed the new binary sensor and all three
  enabled Thread sensors: channel 25, network name and role Router. It still
  classified the Matter transport as Wi-Fi. No companion integration was added.
- A further bounded 180-second observation retained healthy source/router
  state and RTSP DESCRIBE returned 200. Fresh clear motion was observed.
  The initial unattended window did not include a physical motion event.
- A subsequent attended movement test produced two detected-then-clear pairs
  on the original native HA binary sensor. HA recorded Detected at 14:51:49
  and Clear at 14:53:02, then Detected at 14:53:29 and Clear at 14:53:38
  (Europe/Amsterdam). Camera-side fresh native observations independently
  changed true to false. No synthetic events, producer-setting changes, new
  pairing or companion integration were used. This qualifies that bounded
  end-to-end event path, not general detection accuracy or a fixed clear delay.

This remains a candidate, with no stable release or certification claim. For
recovery use the guarded installer's verified previous application slot; no
radio or vendor-camera binary was modified by this update.
