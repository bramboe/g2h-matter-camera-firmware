# Permanently disable the vendor HomeKit (HAP) at boot

This disables the Aqara G2H's built‑in HomeKit AccessoryServer with a **minimal,
size‑preserving binary patch** — three Thumb `CBZ r0` instructions replaced with
`NOP` (`0xBF00`) — that forces the vendor's own *"AccessoryServer will not start"*
code path. RTSP and the Matter pipeline are untouched.

**You apply this to the camera binary already on your own device.** No vendor
binary is distributed here.

## How the G2H builds its running camera at boot

`/etc/tar_app.sh` (run from `S90app` at every boot):

1. extracts `/local/app.tar.xz` → `/tmp/out` (tmpfs, volatile),
2. rewrites hard‑coded cloud IPs to loopback and swaps `miio_task`→`rtsp_task`
   with a **`hexdump -vC | sed | hexdump -R`** round‑trip (size‑checked), and
3. calls a secondary‑encoder `boot` that adds the Constrained‑Baseline 720p
   encoder the Matter/WebRTC path needs, producing the final `/tmp/out/camera`.

Because `/tmp/out` is rebuilt every boot, the persistent change lives in the boot
scripts under `/customer`, not in `/tmp`.

## The patch

Three gate call sites (`bl <hap_disable_check>; cbz r0,<start>`) in the 4.5 MB
vendor camera binary (the state at `/tmp/out/camera` just after step 2 above,
md5 `63a5b004…`). File offsets and the exact `hexdump -vC` lines (each is a
**unique** 16‑byte‑aligned line, safe to `sed`):

| site | file offset | old (CBZ) | new (NOP) |
|------|-------------|-----------|-----------|
| A | `0x1d14be` | `… 05 46 d8 b1` | `… 05 46 00 bf` |
| B | `0x1d1658` | `24 f0 08 f9  c0 b1 …` | `24 f0 08 f9  00 bf …` |
| C | `0x1d4610` | `f8 b1 02 f0 …` | `00 bf 02 f0 …` |

`sed` substitutions for the `hexdump -vC` round‑trip (full lines):

```
s/25 f0 c0 ff 05 f0 82 fa  24 f0 d6 f9 05 46 d8 b1/25 f0 c0 ff 05 f0 82 fa  24 f0 d6 f9 05 46 00 bf/
s/05 f0 b4 f9 24 f0 08 f9  c0 b1 05 f0 95 f9 01 21/05 f0 b4 f9 24 f0 08 f9  00 bf 05 f0 95 f9 01 21/
s/f8 b1 02 f0 b9 f9 42 46  3b 46 01 21 22 f0 fe fd/00 bf 02 f0 b9 f9 42 46  3b 46 01 21 22 f0 fe fd/
```

Patching the 4.5 MB input with these NOPs yields md5 `dde1f2ec…`. Running the
unchanged secondary‑encoder patch (`patch.bin`, applied by `patch-helper`) on top
yields the final 16.8 MB binary md5 `30ca67f7…` — the NOP offsets and the
encoder‑patch offsets don't overlap, so the two operations commute.

## Files here

- `patch.bin` — the secondary‑encoder patch (`G2HBLP01` format: our offline‑built
  PHDR+stub; contains no vendor `.text`).
- `patch-helper` — static ARM applier for `patch.bin` (`helper <src> <patch.bin>
  <out>`; it only operates on `/tmp/out/camera` and a `/tmp/g2h_vendor_baseline_*`
  output path).

## Install (at a full cold boot — never by killing the running camera)

The safe, reversible method: install a one‑shot boot hook that

1. restores the original `/etc/tar_app.sh` first (so a second boot is always
   clean), then arms a **guardian** that reboots back to the known‑good boot
   within ~300 s unless the new boot is committed;
2. verifies `/tmp/out/camera` is the expected input (`63a5b004…`);
3. applies the three NOPs with the `hexdump`/`sed`/`hexdump` round‑trip above,
   checking size (`4546596`) and md5 (`dde1f2ec…`), restoring on any mismatch;
4. runs `patch-helper` with the unchanged `patch.bin` → verifies md5 `30ca67f7…`;
5. installs it as `/tmp/out/camera`.

Reboot once. Verify: both RTSP streams up (Main 1080p + Baseline 720p), Matter
ready, HomeKit gone. Only then write the `committed` flag so the guardian stops
reverting. Keep the original `/etc/tar_app.sh` backup for rollback.

### Rollback

Restore the original `/etc/tar_app.sh`, `sync`, reboot. The camera re‑extracts
the pristine binary from `/local/app.tar.xz` (never modified) and HomeKit returns.

## Notes

- Repeated short RTSP connections can momentarily lock the vendor RTSP server; a
  reboot clears it. Commit the install based on the stream/decode/Matter checks,
  not on an RTSP‑reopen stress test.
- Do not kill the running vendor camera to swap the binary — apply only during a
  full boot, before the camera process starts.
