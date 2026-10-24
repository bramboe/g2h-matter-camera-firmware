# Improvement candidates — 2026-10-24

These changes are separate from the historically tested prebuilt application.
Source/host tests and ARM compilation do not by themselves qualify hardware.
Actual candidate results and remaining checks are recorded in
[the qualification report](TEST-REPORT-2026-10-24.md).

| Area | Candidate behavior | Qualification still needed |
|---|---|---|
| Thread use | Counts/tables for attached children, neighbors, known routers; raw MAC activity and uptime, independent freshness | Real UART samples and large Matter table delivery into HA |
| Media reliability | Separate private Unix-socket broker retains native RTSP ownership across Matter app restarts; bounded buffers discard slow subscribers | Real repeated audio/video starts, app restarts, disconnections and soak |
| Talkback mute | Invalidate queued/partial codec audio across mute and Matter privacy; no stale command dispatched after the mute barrier | Audible device test; an already accepted chunk may finish for 0.512 s |
| Snapshots | Advertise 320×180 and 640×360; actual dimensions validated, per-resolution cache; bounded 64,000-byte JPEG | Actual camera CPU/latency and controller requests; large high-entropy JPEGs can fail closed |
| Motion | Exact-hash/PID/lifetime-bound read-only observation of proven native held-motion fields, exposed through development vendor cluster | Native producer availability and actual motion/clear transition on hardware |
| Updates | Separate slot, fresh private backup, unchanged identity/state, independent timeout and boot rollback | Physical disconnected-install, restart and power-loss qualification |
| Build/release | Pinned inputs, source and link-input provenance, source/relink bundles and validation | Independent rebuild on a second host and full release qualification |

## Privacy scope

Matter soft livestream privacy suppresses this application's media output and
flushes pending talkback. It does not stop the sensor, native RTSP or existing
vendor capture. Analysis proves `video_on=0` gates motion reporting rather than
all H.264 publication. No global camera-off control is advertised on that basis.
A vendor microphonemute acknowledgement is also insufficient evidence of actual
silence. See [VENDOR-MEDIA.md](engineering/VENDOR-MEDIA.md).

## Motion is genuine, but firmware-specific

Cluster `0xFFF1FC12` on endpoint 0 carries read-only nullable native held-motion,
producer enable state, reason and freshness. It is a development vendor cluster,
not a claim of standard Occupancy Sensing conformance. The exact supported vendor
executable SHA-256 is in the observer source. Unknown firmware, disabled producer,
changed/reused PID, inaccessible memory or expired data becomes unavailable.
Nothing enables the native producer or attaches a debugger. Internal motion
reporting retains its native hold behavior; it is not occupancy or person/AI detection.

The current pinned SDK exposes a Vision feature but retains mandatory legacy
sensor-type fields. Their compatibility semantics need normative verification
before advertising a standard motion/occupancy endpoint. Native `_hap` discovery
suppression does not itself disable the internal producer.

## Home Assistant and one pairing

The existing project camera extension is included under
`home-assistant/custom_components/g2h_matter_camera`. Its camera pairing and
unique IDs remain, and Thread role/channel IDs are preserved. Additional Thread
and native-motion entities attach to the same existing Matter device. Diagnostics
use HA's current official Matter client; the media path retains the existing
project signaling implementation. There is no MQTT or extra commissioning.

The optional `g2h_thread_diagnostics` component is for users who only want Thread
diagnostics. Installing it alongside the combined camera extension duplicates
those diagnostic entities and is unnecessary.

MAC packet counts include local/control traffic: they do not prove forwarded
traffic or how much a particular end device used this router. Attached children
and neighbor links provide concrete topology evidence. No Border Router, PTZ,
AI tracking, global capture-off or higher live encoder profile is claimed.
