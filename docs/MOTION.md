# Native motion through the existing Matter pairing

The unified `g2h_matter_camera` HA extension 0.4.0 adds a normal **Motion** binary
sensor to the same Matter device as the camera. It uses the existing Matter
node and fabric. There is no second pairing, MQTT connection, RTSP analysis or
new image-recognition workload. Existing camera and Thread entity identities
are retained.

This is the vendor camera's **held-motion signal**, not occupancy or a promise
of human/pet classification. The observer reads a narrowly verified native
producer; it does not enable or change that producer. If the native producer is
disabled, unknown, stale, or fails executable/layout validation, HA reports the
motion entity as **unavailable**. It never turns those cases into “clear.” A
fresh, enabled producer reporting false is a real clear sample.

The observer is experimental. Merely installing the HA entity does not prove
that native motion is being produced on a particular firmware build. Consult
its `reason`, `producer_enabled` and `freshness_window_ms` attributes;
`sample_age_ms` is available only in downloaded diagnostics. Reasons such as `native-producer-disabled`, `stale`,
`executable-layout-mismatch`, `executable-hash-mismatch`, `hash-timeout` or
`memory-read-failed` describe unavailable observations. No hash, process memory,
network key or private media is exported to HA.

## Interface

Vendor cluster `0xFFF1FC12`, endpoint 0, revision 1:

| ID | Meaning |
|---|---|
| 0 | Fresh known held-motion sample (boolean) |
| 1 | Held motion (nullable boolean) |
| 2 | Native producer enabled (nullable boolean) |
| 3 | Bounded static status reason |
| 4 | Source label: `vendor-vision-held-motion` |
| 5 | Sample age in milliseconds (nullable uint32) |
| 6 | Freshness window in milliseconds (2000) |

HA polls explicit attributes 0/1/2/3/5/6 every half second (the native observer samples once per second).
The coordinator uses a precise relative monotonic timer because HA 2026.10
rounds the normal coordinator schedule to whole seconds. It keeps one pending
poll, waits at least 500 ms after a completed read, honors disabled polling and
longer retry delays, and cancels its timer/read task on unload. It does not
catch up overdue polls in a burst. A local monotonic
expiry also marks the entity unavailable when sample age exceeds the stated
window, even if the following server request is slow or disconnected. Scheduled
expiry and coordinator polling are stopped when the extension unloads; the
shared official HA Matter client is never closed by this extension.

The device reports state/producer/reason changes through Matter. Merely
refreshing the sample timestamp does not dirty the age attribute or advance the
cluster data version. Age is still encoded freshly on an explicit diagnostic
read, and stale/failed producer transitions still report unavailable.

Polling refreshes private timing information without publishing it as entity
attributes. HA is notified only when meaningful motion/producer/status values
change. An actual expiry publishes unavailable once; a fresh sample recovers
once even when its motion boolean is the same as before the expiry.

An unsupported motion cluster on older device software leaves this entity
unavailable while the existing camera and Thread sensors continue loading.
The component does not change HA's Matter subscription or the controller's
cached device-availability flag.

## Offline validation

```sh
python3 tests/test_camera_diagnostics.py
python3 tests/test_camera_platforms.py
python3 tests/test_camera_unload.py
python3 tests/test_camera_config_flow.py
python3 tests/test_motion_precise_schedule.py
```

The fixtures verify false versus null, disabled/stale/malformed samples,
independent optional Thread failures, preserved existing device/entity IDs,
server disconnects, bounded optional reads, local expiry and clean unload.
Hardware motion-event accuracy and uninterrupted live-video behavior require
separate runtime verification; these fixtures are not such a verification.

The historical `producer_enabled` field is the native HomeKit motion-reporting
permission (camera active and not manually disabled). It does not, by itself,
prove that the YUV detector is running or that a detection region is configured.
A fresh clear sample is the native held flag; qualification must include an
actual detected-to-clear transition. No native detector setting is changed.

`tests/test_motion_reporting.cpp` also checks the real SDK cluster data version:
steady motion with changing ages emits no change, while detection, expiry,
recovery and a disabled producer each advance the version. Compile it against
the production SDK with assertions enabled (`-UNDEBUG`). The ARM test was run
on the camera without reading native process memory or altering camera state.

Live qualification still observes occasional unavailable/recovered transitions
under load at the strict two-second freshness deadline. These are distinct from
unchanged-age notification churn. Do not present an expired sample as clear or
claim this candidate has completed motion availability qualification.
