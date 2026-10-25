# Motion freshness follow-up — 2026-10-25

This records a new, not-yet-installed source candidate. The successfully tested
installed app and audible microphone check remain recorded in
[the earlier report](TEST-REPORT-2026-10-24.md).

## Measured cause

Safe HA timing diagnostics on the earlier two-second app recorded 47 reads,
one local expiry during an in-flight read, no native stale samples and no read
errors. The slowest response took 1,190 ms. The expiry occurred at sample age
2,000 ms. This establishes a controller expiry during transport delay in that
interval, not a failed native motion observer. It does not rule out other
failure modes over longer periods.

## New candidate

- App SHA-256: `5612491306a17a30a622e03b1624238fb8d53015c2a3590432795461d7044526`.
- Broker unchanged: SHA-256 `1e7238dea139155d6c8b67fdf4d87a38f784af6a62747fb950c9fcab0ea4736c`.
- The device advertises a four-second freshness lease, allowing the one-second
  producer cadence, two-second read timeout and scheduling margin.
- HA subtracts the complete read round trip from the remaining lease. A slow
  response cannot extend freshness; disabled/failed/expired samples remain
  unavailable, never a fabricated clear state.
- Safe downloaded timing counters make controller expiry, transport errors and
  native stale observations distinguishable without adding entity churn.

## Completed offline checks

- 112 host tests passed, one optional real-codec test skipped in the default run.
- Additional assertions cover read/expiry timing, conservative lease consumption
  and excluding unexpected or noninteger values from exported diagnostics.
- Native observer regression passed with AddressSanitizer and UndefinedBehaviorSanitizer,
  including the exact freshness boundary and process/hash/permission guards.
- Production-SDK ARM observer/reporting tests compiled and linked with assertions
  enabled. They have not yet run for this new candidate on the camera.
- ARM app built from the pinned inputs; extracted relink objects/libraries
  reproduced the exact SHA-256 above. Corresponding source/relink bundles and
  a candidate manifest were created locally.

## Runtime qualification pending

The camera became unreachable over both Telnet and Matter before the new app
was installed or either ARM test was uploaded. Only the diagnostic timing
instrumentation had been loaded in HA; its motion behavior was unchanged.
The new conservative HA timing behavior is prepared but not loaded. The running
app was still the earlier image. The cause of the loss of reachability is not
established; physical power/network confirmation is pending.

After reachability returns: run the temporary ARM tests, create a fresh verified
backup, use the guarded separate-slot updater, load the HA timing change, and
observe availability during idle/live/snapshot operation. Verify genuine
motion transitions and actual unavailable behavior after a bounded read failure.
No camera image, device address, setup code, dataset or credential is included.
