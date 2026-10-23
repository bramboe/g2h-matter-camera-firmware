# G2H Matter-camera 1.0

Eén Matter-apparaat van de Aqara Camera Hub G2H: video, snapshots, microfoon,
terugpraten en camerabediening, alles via één Matter-koppeling. De JN5189 blijft
een zelfstandige Thread-router (FTD), geen Border Router. Experimentele software,
geen gecertificeerd product.

## Wat werkt (getest via Matter)

- **Video** — H.264 720p via WebRTC (Offer/Answer over Matter CASE, DTLS/SRTP).
- **Snapshots** — JPEG 320×180, gedeelde ingest zodat live video niet hapert.
- **Microfoon** — Opus mono 48 kHz, omgezet uit de AAC-bron van de camera.
- **Terugpraten** — half-duplex, Opus in, afgespeeld via de speaker van de camera
  (ongeveer 0,5–1 s vertraging).
- **Bediening**
  - Standaard Matter-attributen: nachtzicht (Uit/Aan/Auto), statuslampje,
    HDR (WDR), speakervolume, dempen microfoon/speaker.
  - **Privacymodus** (SoftLivestreamPrivacy): de app stopt met video én geluid via
    Matter; de vendor-stream, RTSP en HomeKit blijven ongemoeid. Omkeerbaar.
  - G2H-vendorcluster `0xFFF1FC11`: helderheid, tijdstempel-in-beeld (OSD),
    speakervolume. Standaardcontrollers tonen dit cluster niet.
- **Thread** — G2H-vendorcluster `0xFFF1FC10`: rol, kanaal, netwerknaam enz.

## Wat (nog) niet werkt of ontbreekt

- **Weergave in Home Assistant** — HA toont Matter-camera's pas vanaf HA 2026.11
  met een Matter-server van schema 14. Daarna verschijnt de camera vanzelf in het
  bestaande Matter-apparaat; opnieuw koppelen is niet nodig.
- **Beeld spiegelen/draaien** — geen veilige live-instelling; alleen een
  configuratiesleutel die een herstart van het camera-programma vereist. Niet gebouwd.
- **Opname (recording), globale hard-privacy, Wi-Fi via Matter** — niet van toepassing.

## Bekende bijwerking

Stopt de Matter-app na een audiosessie (bij een update, herstart of reboot), dan
herstart het camera-programma van Aqara eenmalig. RTSP en HomeKit komen vanzelf
terug. In normaal gebruik stopt de app niet.

## Inhoud van dit pakket

- `g2h-matter-camera` — de ARM-binary (zie SHA256SUMS).
- `installatie/` — scripts om te installeren, te herstellen en de host te controleren,
  met de benodigde hulpmodules.
- `bron/` — onze eigen broncode (de Matter-app, geen vendor-code).
- `licenties/` — LGPL-licenties en bronverwijzingen voor FFmpeg en Opus.
- `INSTALLATIE.md` — stap voor stap installeren.
- `KOPPELEN.md` — de camera in Home Assistant koppelen.
- `SHA256SUMS` — controlehashes.

## Licenties

De app linkt statisch met FFmpeg (LGPL v2.1) en Opus (BSD). Zie `licenties/`. De
volledige, herbouwbare bronnen van FFmpeg 8.1.3 en Opus 1.6.1 horen bij een
verspreide release; de herkomst staat in `licenties/herkomst.json`.
</content>
