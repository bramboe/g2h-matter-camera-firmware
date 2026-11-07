# Disable the vendor HomeKit at boot (verified method)

This makes the Aqara G2H no longer discoverable as a HomeKit accessory, so it
disappears from Home Assistant / Apple Home and can no longer be paired over
HomeKit. It is a **minimal, size‑preserving one‑byte patch** to the vendor
camera binary, applied at boot. RTSP and the Matter pipeline are untouched.

**You apply this to the camera binary already on your own device.** No vendor
binary is distributed here.

## What actually works — and what didn't

HomeKit controllers discover accessories purely via the **mDNS service type
`_hap._tcp`**. The reliable disable is to **corrupt that service‑type string** in
the camera binary so the camera advertises an unrecognized type (`_xap._tcp`):
every HomeKit controller browses for `_hap._tcp`, so it simply never sees the
device. Verified on a live G2H — after the patch an mDNS browse shows **no
`_hap` advert** for the camera (only the inert `_xap`), and the HA card clears.

> Honest note: an earlier attempt to force the vendor's "AccessoryServer will not
> start" path by NOP‑ing three `CBZ` gate instructions did **not** work — those
> gates were not the control point, and `homekit_task` kept running and
> advertising. The `sys.hap_disable` / `persist.sys.hap_disable` property had no
> effect either. The service‑type‑string patch below is the method that was
> actually verified to remove the advertisement.

The HAP server process still runs internally (its port stays open), but with no
`_hap` advertisement it is no longer discovered as a HomeKit accessory. The HAP service and existing pairings remain; this patch does not stop the HomeKit stack.

## The patch

In the 4.5 MB vendor camera (the state at `/tmp/out/camera` just after
`tar_app.sh`'s own patches, md5 `63a5b004…`), the null‑terminated service‑type
string `_hap._tcp` that is loaded into the mDNS register call lives at **file
offset `0x3c0a91`**. Change the `h` (`0x3c0a92`, `0x68`) to `x` (`0x78`):

```
_hap._tcp  ->  _xap._tcp
```

`hexdump -vC | sed | hexdump -R` substitution (the vendor's own technique; the
target line is unique in the file, byte 2 of the 16‑byte line):

```
s/00 5f 68 61 70 2e 5f 74  63 70 00 52 65 67 69 73/00 5f 78 61 70 2e 5f 74  63 70 00 52 65 67 69 73/
```

Hash chain (reference): vendor input `63a5b004…` → after the one‑byte change
`04c86ff9…` → after the secondary‑encoder patch (`patch.bin`, applied by
`patch-helper`) `5974b4be…` — the final 16.8 MB binary installed at
`/tmp/out/camera`. The string offset and the encoder‑patch offsets don't overlap,
so the two operations commute.

## Files here

- `patch.bin` / `patch-helper` — the secondary‑encoder patch and its ARM applier
  (needed for the Matter/WebRTC 720p encoder; see `../docs/OVERVIEW.md`). The
  HomeKit disable is the string change above, applied to the input *before* this.

## Install (at a full cold boot — never by killing the running camera)

Install a one‑shot boot hook that

1. restores the original `/etc/tar_app.sh` first, then arms a **guardian** that
   reboots back to the known‑good boot within ~300 s unless the new boot is
   committed;
2. verifies `/tmp/out/camera` is the expected input (`63a5b004…`);
3. applies the `_hap`→`_xap` change with the `hexdump`/`sed`/`hexdump`
   round‑trip above, checking size (`4546596`) and md5 (`04c86ff9…`), restoring
   on any mismatch;
4. runs `patch-helper` with the unchanged `patch.bin` → verifies md5 `5974b4be…`;
5. installs it as `/tmp/out/camera`.

Reboot once. **Verify with an mDNS browse** that `_hap._tcp` is no longer
advertised for the camera (e.g. `dns-sd -B _hap._tcp local.`). Only then commit
so the guardian stops reverting. Keep the original `/etc/tar_app.sh` (and the
previous boot script) backed up for rollback.

## Rollback

Restore the original `/etc/tar_app.sh` (and the previous boot script), `sync`,
reboot. The camera re‑extracts the pristine binary from `/local/app.tar.xz`
(never modified) and HomeKit returns.

## Notes

- Verify by behaviour (mDNS browse / the HA card), not by a thread‑name grep — a
  running HAP thread can momentarily disappear during boot and read as "off".
- Do not kill the running vendor camera to swap the binary — apply only during a
  full boot, before the camera process starts (the video hardware does not
  re‑initialise reliably otherwise).
