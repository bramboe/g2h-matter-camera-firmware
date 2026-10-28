# Internal Thread-router diagnostics

**Native-only default (0.5.4 candidate):** channel, network name and routing
role are available on the existing HA Matter device with no G2H component.
See [NATIVE-DIAGNOSTICS.md](NATIVE-DIAGNOSTICS.md). The richer topology/counter
entities described below require an optional companion and are not part of
that native-only setup.

The JN5189 remains a standalone Thread Full Thread Device. It routes inside its
existing Thread mesh; the G2H Linux host is **not a Thread Border Router**. The
camera and diagnostics share the existing Matter node and pairing. No MQTT,
second fabric, commissioning, or Thread dataset is needed for this extension.

## What demonstrates usage

- **Attached children** proves that end devices selected this router as parent.
- **Neighbors** shows direct Thread links, with average/last RSSI and age in the
  corresponding table. Router-table link-quality fields are values 0–3.
- **Known routers** is the radio's router table, including remote routes and
  potentially its own router. It is not the number of clients served.
- **MAC TX/RX/retry totals** show local radio activity. They include control
  traffic and the router's own packets. They **do not measure forwarded traffic**
  or prove that a particular Matter device's video/control passed through G2H.

No per-route forwarding counter is exposed by the existing read-only CLI used
here. This implementation makes no such claim. Exact forwarded-byte accounting
would require an independently reviewed radio-firmware change.

## Firmware implementation

The host polls the original seven identity/status commands and `ifconfig`, then the optional
read-only commands `neighbor table`, `child table`, `router table`, `counters mac`
and `uptime ms`. Formats were checked against the repository's existing NXP
OpenThread `src/cli/cli.cpp` implementations of those commands. The CLI command
names themselves are read-only. Explicit enable control, described below, is the
only path that sends configuration commands. No `reset`, key/dataset, GPIO, or
flashing command is sent. UART parameters are restored after each operation,
and the existing cooperative UART lock remains in use.

Each response needs its exact echoed command and final `Done`. Responses are
bounded to 16 KiB/66 lines, line length 1024, and three seconds per command;
tables have at most 64 entries and 12 KiB of validated text. A missing command,
`Error`, truncated output or malformed row invalidates only that optional
sample. It does not fabricate an empty table or zero counters. Complete empty
tables are valid. Each table and the counters have independent 90-second
freshness; expired/unavailable values read as Matter null.

Raw 32-bit counters and 64-bit radio uptime are exposed. No rate or accumulating
HA `total_increasing` statistic is advertised: a counter decrease might be
wraparound, reboot, or manual reset. The supplied delta utility deliberately
drops that interval and also rejects expired samples or uptime regression.

## Matter interface

Vendor cluster `0xFFF1FC10`, endpoint 0, revision 3. Existing IDs 0–23 retain their
original types and behavior. These fields are appended:

| ID | Type | Meaning |
|---|---|---|
| 9 / 12 / 15 | boolean | Fresh neighbor / child / router table |
| 10 / 13 / 16 | nullable uint16 | Neighbor / attached child / known router count |
| 11 / 14 / 17 | nullable string | Corresponding validated CLI table |
| 18 | boolean | Fresh MAC counters and uptime |
| 19 / 20 / 21 | nullable uint64 (raw 32-bit values) | MAC TX total / RX total / TX retries |
| 22 | nullable uint64 | Radio uptime in milliseconds |
| 23 | uint16 | Freshness window in seconds (90) |
| 24 | nullable boolean; writable boolean | Explicit requested Thread enabled preference; null until requested |
| 25 | nullable boolean | Observed Thread enabled state, independently verified and fresh |
| 26 | boolean | An explicit enable/disable request is pending |
| 27 | string | Empty for no control error; otherwise a bounded local error category |

Tables include radio addresses and mesh topology. Treat exported HA diagnostics
as personal network metadata. They contain no network key or dataset.

## Explicit Thread enable/disable control

`ThreadRadio::SetEnabled(bool)` queues a request on the existing polling worker;
it performs no UART I/O on the Matter event loop. Disabling sends `thread stop`
and then `ifconfig down`. Enabling sends `ifconfig up` and then `thread start`.
Both exact command acknowledgements and a subsequent `state`/`ifconfig` readback
are required. Disabled means Thread state `disabled` **and** interface `down`;
enabled means interface `up` and Thread state `detached`, `child`, `router` or
`leader`. Enabled does not by itself mean attached to the mesh. Mixed, malformed,
missing or expired observations are unknown, never silently off or on.

The write response acknowledges a queued request, not physical success. Clients
should display attribute 25 and consult pending/error attributes 26–27. An
unsuccessful operation consumes that attempt and reports `uart-busy`,
`uart-unavailable`, `uart-settings`, `command-failed` or `verification-failed`.
It does not repeatedly replay a failed command during every telemetry poll.
A new explicit request can retry. Latest queued requests win; a newer request
arriving during an older operation remains pending.

Construction and ordinary polling make no configuration change. The cluster
accepts an optional `bool(bool)` persistence callback. If supplied, it must save
an explicit preference before queuing the request; failure rejects the Matter
write without touching the radio. The app owner can restore that saved preference
using the same `SetEnabled` API before starting the worker. No callback means
runtime-only control. No Thread dataset, commissioning state or radio firmware
is erased or rewritten. Polling and control share the same in-process mutex,
cooperative UART directory lock and serial-setting restoration. The Matter
camera is still on the host's existing IP connection, so stopping this separate
Thread mesh router is not a Matter fabric removal.

Host validation includes a real PTY CLI peer for command order, verified
disable/enable, partial failures, false acknowledgements, UART-lock contention,
latest-request and in-flight replacement, and worker wakeup. These new control
paths still require guarded physical-device validation before release. Run:

```sh
clang++ -std=c++17 -pthread -fsanitize=address,undefined tests/test_thread_control.cpp -o /tmp/g2h-thread-control-test
/tmp/g2h-thread-control-test
```

## Home Assistant: use the existing camera extension

Version 0.4.0 of `home-assistant/custom_components/g2h_matter_camera` includes the
Thread diagnostics and a native held-motion binary sensor alongside the camera.
Upgrade that component in place and restart/reload its existing HA config entry.
The camera entity, node selection, device identifier and existing role/channel
unique IDs are preserved. This uses the already paired Matter node; no second
commissioning or MQTT is involved. If the extension was not installed, add
**G2H Matter Camera** and select the existing paired G2H once.

Counts, role, channel, raw MAC totals and uptime are diagnostic sensors. Count
sensors include their table as the `radio_table` state attribute. Optional
missing/stale fields are unavailable, never zero; an unavailable motion cluster
on older firmware does not stop the camera or Thread sensors loading. Motion
is a normal Motion entity, distinct from occupancy. It reports real clear
samples as false and missing/disabled/stale samples as unavailable. Local expiry
also prevents a slow Matter-server read from keeping motion available beyond
the producer's stated freshness window.

`g2h_thread_diagnostics` remains an alternative for users who only want Thread
diagnostics. Do **not** install both components for the same device unless you
intentionally want duplicate diagnostic entities. Both use HA's existing
Matter client and never pair devices. The camera extension can request the
explicit Thread enable control above; the diagnostics-only component remains
read-only.

## Validation and limits

Run these from the repository root:

```sh
clang++ -std=c++17 -fsanitize=address,undefined tests/test_thread_telemetry.cpp -o /tmp/g2h-thread-test
/tmp/g2h-thread-test
python3 tests/test_thread_ha.py
python3 tests/test_thread_ha_client.py
python3 tests/test_camera_diagnostics.py
python3 tests/test_camera_platforms.py
python3 tests/test_camera_unload.py
python3 tests/test_camera_config_flow.py
python3 tests/test_camera_availability_events.py
```

The host tests cover real CLI table shapes, empty/malformed/truncated rows,
RSSI ranges, counter overflow/duplicates/missing fields, stale samples, wrap and
reboot ambiguity, and a PTY fixture with old asynchronous lines, echoed commands
and unsupported optional commands. HA validation tests cover unavailable,
malformed and older field sets. The complete app object target also compiles through the public ARM CMake
recipe and the pinned local Matter SDK, including nullable attribute encoders.
The extension uses explicit core attribute reads first, then a separate
bounded optional numeric group and independent table reads. A failed or large
table cannot hide a valid role/channel sample; the total polling deadline is
under ten seconds. Motion is documented in [MOTION.md](MOTION.md).

The extension now uses HA's existing official Matter client `read_attribute`
method directly; its method/helper signatures were checked against the cached
local HA/Matter-client sources. Three offline client tests verify the read-only
call, existing-entry binding and error handling. These additions have **not yet
been verified on the physical radio or against a running HA instance**. The
12-KiB table attributes require a working Matter large-message/TCP path;
end-to-end table delivery is unverified. Guarded hardware validation is required
before release; public prebuilt binaries do not automatically include source
changes.

Unified-extension lifecycle/error tests also cover older firmware with an
unsupported motion cluster, independent stale radio fields, preserved device
and entity identity, server disconnection, local motion expiry and coordinator
shutdown. HA's coordinator shutdown API was checked against the official
[HA 2026.10.2 source](https://github.com/home-assistant/core/blob/2026.10.2/homeassistant/helpers/update_coordinator.py).

## Public runtime diagnostics

The unified camera extension supports HA's **Download diagnostics** action.
It reports cached device availability separately from two explicit core-status
reads: HA's existing official Matter client and an independent connection to
that same Matter server/fabric. This helps distinguish stale controller state,
read failures and a genuinely stale radio sample. Probes are read-only and
bounded; neither pairs nor modifies a device.

The export includes only selected public state/counts, table lengths and error
class names. It excludes server URLs, entry data, fabric/dataset identifiers,
network names/addresses, table contents, raw errors, SDP and media. Offline
privacy and cleanup tests run with
`python3 tests/test_camera_runtime_diagnostics.py`.

At setup, the extension first verifies the configured camera identity with a
live CASE read. If that succeeds while HA's cached Matter node is offline, it
requests one bounded (15-second) standard node interview to refresh the
controller cache. Failure is nonfatal for Thread and motion setup, and neither
global subscription settings nor device attributes are changed. Diagnostics
record only the attempt/result, resulting cached availability and error types.
An already online or unverified device is never interviewed by this path.

The camera entity listens to the existing HA Matter client's cached
`NODE_UPDATED` events so an authentic availability change is republished even
with camera polling disabled. The listener is removed on entity unload. It
creates no additional device subscription and never fabricates availability.

## Bounded table previews

Counts are computed from the complete validated radio tables. The optional
text attributes 11, 14 and 17 contain at most 768 bytes, ending at a whole
line and marked `[truncated; count is complete]` when shortened. A Matter UDP
report cannot fragment one oversized string; previously a larger table could
return ResourceExhausted and interfere with a full interview. These attributes
are previews, not a promise of a complete neighbor/router list. Counts and
freshness remain independent of the preview.
