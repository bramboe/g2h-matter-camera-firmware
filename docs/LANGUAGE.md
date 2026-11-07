# Language selector: English and Dutch

Candidate 0.5.6 adds **Language / Taal** to the existing unbridged Matter node,
using standard Mode Select on endpoint 7. No companion HA integration, extra
pairing, MQTT account or second HA device is required. The same selected locale
is also exposed through root Localization Configuration (`0x002b`): `en-US`
and `nl-NL` are the only supported ActiveLocale values.

After adding this new endpoint, re-interview the existing device and reload
only the existing HA Matter integration once. The initial interview exposed
the selector but HA retained old values until that reload; subsequent EN/NL
changes updated all six descriptions/options automatically. No new pairing or
HA restart was needed.

## Scope

The selection translates the six firmware-owned Mode Select descriptions and
options: Thread radio, Camera audio, Camera video (restart), Night vision,
Status light and Date and time overlay. Numeric mode values and endpoint IDs
are unchanged. English/Nederlands endonyms keep the language selector usable
in either language. HA's own interface, generic volume/diagnostic labels and
names a user has overridden still follow HA's profile/entity registry settings.

The option strings themselves change between languages. HA automations that
compare literal strings such as `Enabled` or `Automatic` need to account for
`Aan` or `Automatisch`; entity IDs and numeric Matter mode values stay stable.
Changing language never changes a privacy control, the radio state or volume.

For speech, two private banks contain the 16 status prompts that the existing
camera resolves through its localized voice path. The Dutch bank uses local
synthetic speech; the English bank retains the owner's factory files unchanged.
Each bank also has a `language_test.aac` for an attended test. Selecting a
language does not itself play audio or generate camera notifications. Legacy
Zigbee prompts do not acquire new meaning or events in the Thread firmware.
Unlocalized tones and other files outside those 16 prompts are not translated.

This feature does not create a HA `media_player`, add music playback or make the
speaker a TTS destination. The existing native Speaker component remains a
volume control; tested Matter talkback is a separate camera capability.

## Private preparation and installation

Voice assets are deliberately absent from the public repository. Start with
English files from the same owned camera and keep a verified private backup.
On macOS, `tools/prepare_voice_bank.py` uses the already-installed `say` voices
and `ffmpeg`; it does not install software, contact the camera or play sound.
Use its `--help` for input/output paths. Keep generated assets private.

`install/g2h_voice_banks_live.py` accepts the generated directory and `install/voice_banks_validate.cpp`, built as a static ARM executable with the
same toolchain and OpenSSL crypto library as the app. It verifies the
exact files, bounded AAC-LC 16 kHz mono ADTS frames, per-file hashes and sorted
SHA256SUMS manifests. It does not accept arbitrary archives. The 16 English
files must also match a fresh copy from the target camera before remapping.

The installation preserves the original `/etc/ch/en` directory by renaming it
on the same filesystem. A canonical symlink in the persistent configuration
selects `/mnt/sdcard/g2h-matter-camera/voice/en` or `/nl`; `/etc/ch/en` resolves to
that one pointer. No native executable is patched, process stopped or rebooted.
The SD card must remain inserted. Both banks are completely checked before the
feature is registered and before a language change can be accepted.

A separate 120-second timer restores and verifies the original directory if
installation is not committed. Factory assets and persistent rollback script
remain after success. A power interruption stops that timer: the retained
script/private RECOVERY.txt supplies the terminal recovery route in that case.
This is not an assertion of automatic recovery across an interrupted host boot.

Build the read-only validator after preparing the app dependencies described in
[REPRODUCING.md](REPRODUCING.md). With the pinned ARM compiler wrapper and `ZIG`
already configured, use the build output directory as `OUT`:

```sh
build/toolchain/g2h-cxx -std=c++17 -Os -static -pthread \
  -I "$OUT/crypto-prefix/include" install/voice_banks_validate.cpp \
  "$OUT/crypto-prefix/lib/libcrypto.a" -o "$OUT/voice-banks-validate"
python3 install/g2h_voice_banks_live.py --host CAMERA_IP \
  --banks PRIVATE_BANK_DIRECTORY --validator "$OUT/voice-banks-validate" \
  --backup-root PRIVATE_BACKUP_DIRECTORY
```

Install validated banks before the 0.5.6 app. Without both complete banks and
the exact installed pointers, the app omits the language feature.

## Apply and recovery behavior

The native camera accepts `en`/`cn` on this model, not `nl`. Both banks therefore
use its established English voice path. The service reads `misc language`,
sets it to `en` only when a successful read returned `cn`, and verifies readback.
The unrelated `persist.sys.language` property is not changed.

One durable pointer controls speech and firmware UI locale. Selection runs on a
worker, validates the bank, atomically swaps the pointer and fsyncs its parent,
then confirms the pointer and native getter before publishing the language.
Pending/failed observations do not masquerade as an applied locale. Fsync or
readback failures attempt to restore the previous pointer; further filesystem
failure can defeat restoration, so failure remains unknown until re-observed.
Ordinary native setting exchanges are serialized across workers.

For full rollback, first select the previous own Matter app using the existing
app-update recovery route, then run the exact `rollback.sh force` path recorded
in the private voice-install report. It restores and checks the original voice
directory. Staged banks are retained; the script never removes arbitrary assets.
Shared camera, monitor/watchdog and media-broker services are not restarted.

The new clusters are compatibility implementations, not a claim of complete
Matter model conformance or certification.

## Qualification

On 2026-10-28 the app built from 103 recorded source inputs had SHA-256
`da97332fc3cc2ccc3ea02f0cefe5b3dd3758d9d4ce2ddca8a755f3cb53606bff`
(8,361,968 bytes). All 186 executed host tests passed, with one additional
optional codec test skipped. Two assertion binaries linked against the real
production Matter SDK also ran successfully on the camera.

The exact app was installed to a separate SD slot and passed 45 seconds of
continuous guarded readiness after its initial radio snapshot. Three original
fabrics, the native camera process/host lifetime and the existing media broker
were retained. An earlier attempt hit its startup time budget and restored
0.5.5; the corrected startup allowance retains the independent 180-second guard
and at least 30 seconds reserved before its action.

Through the existing HA 2026.11.0b0 pairing, the actual choice changed
EN → NL → EN → NL. After the one initial integration reload, all six names and
option strings refreshed automatically in both directions. An independent
read-only observer confirmed the matching voice-bank pointer and unchanged
night vision, lamp, OSD, volume and both mute values. The Dutch pointer survived
a restart of only the own Matter app; the native camera lifetime and three
fabrics remained unchanged, and fresh Thread router status returned.

All 34 assets passed production target checks; the 16 installed English prompts
were bit-identical to the private factory backup. Native path resolution and
its required English setting were checked. **Acoustic EN/NL listening was
explicitly deferred by the owner.** No test sound was played, so audible spoken
language is not claimed as physically verified.

Native HA video was then verified again on 0.5.6: decoded 1280×720 playback with
an advancing browser clock, increasing camera frame counters and no media error.
RTSP DESCRIBE returned 200; fresh Thread router state and original fabrics were
retained. The final selected language is Nederlands.
