# Native settings candidate 0.5.5

The qualified installation uses one existing HA Matter device and pairing.
There is no active G2H custom integration, MQTT connection, second device page
or newly commissioned fabric. HA 2026.10.4 and the existing OHF Matter Server
1.5.5 (matter.js 0.18.0, schema 14) were observed during qualification.

## Available functions

| Function | Native interface | Existing camera backend |
| --- | --- | --- |
| WiFi signal | Diagnostic sensor, dBm; disabled by default | Actual negative wlan0 signal in `/proc/net/wireless`, with interface-up check |
| Night vision | Mode Select, Off/On/Automatic, endpoint 3 | `gpio nightvision`, values 0/1/2 |
| Status lamp | Mode Select, Off/On, endpoint 4 | `gpio led`, values 0/1 |
| Date/time overlay | Mode Select, Off/On, endpoint 5 | `video osd`, values 0/1 |
| Loudspeaker volume | Native 0–100% slider, endpoint 6 | `audio out_vol`; neither mute is released |
| Starts, start reason, uptime | Existing diagnostic entities, initially disabled | Matter SDK application diagnostics |
| More Thread information | Matter-server attributes on the same node, endpoint 2 | Existing serialized JN5189 snapshots |
| Snapshot | Existing Matter-server camera UI, endpoint 1 | Existing standard Matter snapshot commands |

After updating an existing pairing, use **Re-interview device**. Enable the
Wi-Fi RSSI, reboot-count, boot-reason and uptime entities in HA's Diagnostic
section. The qualified installation names these WiFi-signaal, Aantal starts
(Matter), Herstartreden (Matter), and Uptime (Matter-app). The old device and
entity identities are retained.

The new settings are components of the same unbridged node. Three utility
endpoints provide the explicit Mode Select descriptions; the real built-in
loudspeaker is described as a Speaker component so HA maps its Level Control
values to a volume slider. No extra speaker or camera device page is created.
These are partial compatibility implementations, not claims of full device-type
or cluster conformance or certification. For example the Speaker projection
does not implement Identify/OnOff or timed level ramps. Unsupported commands and
nonzero transition times are rejected, rather than silently ignored.

Settings use the existing native vendor worker and range checks. A new native
HA request remains unknown until setter acknowledgement and native getter
readback agree. A queued value is not reported as an applied setting. A short
Unknown state can therefore appear in HA during a volume change. The existing
global sound/video/Thread controls and privacy gates remain authoritative.
Video settings are refused while full capture is disabled. Native setters use
their existing persistence path; a new cold-boot persistence trial was not
performed for this candidate.

WiFi values are refreshed every five seconds. Only valid negative dBm values
from -120 through -1 on wlan0 are accepted; the driver's -256 sentinel, positive
quality values, other interfaces, malformed input and a down interface are not
published as signal readings. Missing observations encode null.

## Herstartgegevens are application diagnostics

The Linux SDK uptime measures time since Matter application startup. Its start
counter also increases when the app is restarted for an update; neither is an
independent count of host power cycles. The observed start reason was
Unspecified. Do not interpret it as proof of a watchdog reset, power fault or
the absence of those faults. No hardware reset-cause register is inferred.

## Extra Thread information

In the existing Matter Server UI choose the G2H node, **Endpoint 2 →
ThreadNetworkDiagnostics**. In addition to channel, role and network name,
the candidate exposes these actual observed standard attribute values:

| Attribute ID | Meaning |
| --- | --- |
| 3 | PAN ID |
| 4 | Extended PAN ID |
| 22 (`0x16`) | MAC transmitted-packet total |
| 33 (`0x21`) | MAC transmit retries |
| 39 (`0x27`) | MAC received-packet total |

They reuse the existing radio worker; no additional CLI requests, dataset/key
reads, radio configuration or counter resets were added. Freshness remains
bounded to 90 seconds. Stale counters fail reads instead of becoming zero.
Unchanged samples do not emit repeated value-change notifications.

These counters include control and local traffic. They do not measure forwarded
bytes or prove that another device used this router. HA currently maps only
channel, role and network name to native entities. Other values remain
diagnostics on this same Matter node; no companion component is installed.

The Thread cluster remains a partial compatibility projection, with feature map
0. Other model fields may appear as null or empty defaults in a controller UI.
In particular a default empty NeighborTable/RouteTable is **not** an observed
absence of neighbors/routes. Real richer tables remain in the pre-existing
vendor cluster `0xFFF1FC10` on endpoint 0, for capable diagnostic clients.

## Snapshot using the existing pairing

Open the existing **Matter Server** sidebar page, choose the G2H node, then
**Endpoint 1 → Live View → Snapshot**. A 640×360 JPEG was actually received and
displayed through this route in the qualification trial, without pressing
Start, saving a media file, adding a fabric or installing a G2H component.
The snapshot was then closed. This is the existing server's UI, not a new HA
camera entity or a snapshot button on the ordinary HA device card. That native
HA camera-card integration remains unavailable in the tested HA release.

## Qualification on 2026-10-28

App SHA-256:
`3facc339feb26b84b748911f2874918531507efc346e33ec41c71c18905fe642`
(8,343,376 bytes). The historical public prebuilt is unchanged.

- Pinned SDK/app build passed; all 98 recorded source input hashes matched.
  The build recipe now refreshes staged SDK project inputs and refuses a stale
  header set before building the app. An old metadata/endpoint-limit staging
  mismatch was caught and rebuilt before installation.
- 180 host tests: 179 passed, one optional codec test skipped. Tests include a
  dishonest native setter acknowledgement and rejected/mismatching readback.
- Both native settings and diagnostics assertion executables were linked to
  the production SDK and executed successfully on the camera before installation.
  They cover valid/invalid RSSI, request bounds, actual data-version changes,
  unknown observations and suppression of unchanged-sample notifications.
- A guarded app-only slot installation passed 60 seconds of qualification.
  The previous 0.5.4 slot, original host boot/native camera process, unchanged
  media broker and all three original fabrics were retained.
- HA displayed the volume slider and all new settings on the original Matter
  device. Commands were sent through the existing HA pairing: night vision
  Auto→Off→On→Auto, status lamp Off→On→Off, overlay On→Off→On, volume 80→79→80.
  Independent native read-only observations confirmed the changed settings;
  a final native observation confirmed the original values and both mute flags
  unchanged. No audio test sound was played.
- HA displayed a real WiFi reading (for example -51 dBm), start count 139,
  Unspecified start reason and application uptime. Matter Server displayed the
  new PAN/extended-PAN values and actual TX/retry/RX counters on the same node.
- The existing Matter-server snapshot button returned a decoded 640×360 image.
  No extra test fabric was created. No household image was saved or published.
- A final read-only check confirmed RTSP `/14` DESCRIBE 200, fresh Thread router,
  progressing status time, no source/audio fault, and the original three fabrics.

This is a candidate, not a stable release. Physical optical/acoustic effects,
new-setting cold-boot persistence, long-term stability and HA stale-value/error
display guarantees are not established by these bounded readback tests.
Restore the exact previous application using the guarded installer's verified
backup if the application needs rollback; do not reset pairings or flash the
radio to undo this host-app update.
