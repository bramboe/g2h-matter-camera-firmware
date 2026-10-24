# Updating an installed media broker

The app-slot updater never replaces a different running broker. Closing its
native audio RTSP session can restart the vendor camera. A broker change needs
a **full camera reboot**, a verified private backup, and working Telnet recovery.
Do not kill the broker, shared monitor, or native camera while the host is running.

The new audio filter drains the native audio session but forwards only its
negotiated audio/RTCP channels. The tested vendor also sends video on that
session: a read-only count found 249 video and 51 audio records in 3.176 seconds.
Forwarding those video bursts to an AAC decoder can evict its subscriber. Audio
has a bounded 256 KiB requested socket buffer and a 250 ms write deadline;
video retains its 64 KiB requested buffer and 25 ms deadline. Actual kernel
buffer limits can differ.

## Maintenance procedure

1. Close all Matter live views. Run the host check and backup tools. Keep the
   completed, checksum-verified backup on a second computer. Keep the currently
   selected app and its SD card intact.
2. Verify that no app update transaction is pending and that the installed
   broker matches the saved backup. Stage the new broker under a separate
   filename; verify its exact release-manifest SHA-256 on the computer and its
   size/MD5 after transfer. Never reuse an unverified file from an SD card.
3. Copy the old broker to a unique retained rollback filename under
   `/customer/g2h-matter-camera/`, verify the copy, and check free space. Copy the
   new broker to `g2h-media-broker.next` on that same filesystem and make it
   executable. Sync before atomically renaming it to `g2h-media-broker`.
4. Reboot the entire camera normally. Do not run a warm broker stop/start.
   Verify a changed host boot ID, the same selected app, the exact broker hash,
   preserved Matter fabrics, fresh Thread role, native image hash, RTSP, decoded
   Matter video/audio, and repeated controller reopen operations.
5. If qualification fails, connect through Telnet, copy the verified retained
   broker to the `.next` path, make it executable, sync and rename it back, then
   reboot the camera normally again. The app selector and pairing state remain
   untouched. Keep both copies until the new version passes qualification.

This is an explicit maintenance migration. It does not have the independent
180-second guardian provided for normal app-slot updates. If the camera loses
LAN access during boot, restore connectivity before attempting the Telnet
rollback. A backup is a recovery plan, not proof of power-loss recovery.

See [the current test report](TEST-REPORT-2026-10-24.md) for actual qualification
results; a successful build or local simulator test is insufficient.
