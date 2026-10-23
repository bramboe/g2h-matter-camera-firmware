# Reproductie via de eigen update-weg van de camera (zelfopruimend app.bin)

De camera installeert bij het opstarten zelf een `app.bin` van `/customer/app.bin`
(of `/ota/app.bin`) via `/etc/check_upgrade.sh`: uitpakken → `copy_files.sh` →
`app.bin` verwijderen → herstarten. Geen handtekening nodig. We voegen alleen onze
Matter-service toe; vendor-bestanden (`/local/app.tar.xz`, camera/RTSP/HomeKit)
blijven ongemoeid, dus het is omkeerbaar.

## Zelfopruiming — eindresultaat is een schone, bijgewerkte camera

Na een geslaagde installatie blijft er niets achter:

- **Intern:** `check_upgrade.sh` verwijdert `/customer/app.bin` en `/tmp/upgrade`
  automatisch en herstart de camera.
- **Op de SD-kaart:** onze `copy_files.sh` verwijdert het dragerbestand
  (`/mnt/sdcard/g2h_app.bin`).
- **Resultaat:** een camera met onze Matter/Thread-service actief, zonder enig
  installatie-artefact.

## 1. Bouw het zelfopruimende app.bin (op de Mac)

```bash
B=/tmp/g2h-appbin; rm -rf "$B"; mkdir -p "$B/payload"; cd "$B/payload"
P=~/Documents/Codex/2026-10-19/referenced-chatgpt-conversation-this-is-an/outputs/G2H-Matter-camera-1.0
cp "$P/g2h-matter-camera" camera
cp "$P/../G2H-Matter-camera/S96g2hmattercamera" S96g2hmattercamera
printf '#!/bin/sh\nexit 0\n' > check_coor.sh
cat > copy_files.sh <<'EOF'
#!/bin/sh
# Installeert ALLEEN onze Matter-service en ruimt zichzelf op (intern + SD).
set -e
mkdir -p /customer/g2h-matter-camera /mnt/config/g2h-matter-camera/state
cp /tmp/upgrade/camera /customer/g2h-matter-camera/camera; chmod 755 /customer/g2h-matter-camera/camera
cp /tmp/upgrade/S96g2hmattercamera /etc/init.d/S96g2hmattercamera; chmod 755 /etc/init.d/S96g2hmattercamera
# SD-drager opruimen (intern /customer/app.bin + /tmp/upgrade doet check_upgrade.sh zelf)
rm -f /mnt/sdcard/g2h_app.bin /mnt/sdcard/app.bin 2>/dev/null || true
sync
EOF
chmod 755 camera S96g2hmattercamera check_coor.sh copy_files.sh
tar -czf "$B/app.bin" .
```

## 2. Voorwaarden

- Verse/lege `/customer` (onze binary is ~8 MB). Op een camera waar onze app al
  draait: gebruik de netwerk-installatie (`INSTALLATIE.md`), niet deze weg.
- Per apparaat nog te voorzien (uniek per camera, dus niet in app.bin):
  - `/mnt/config/g2h-matter-camera/identity` — de Matter-identiteit.
  - `/mnt/config/g2h-matter-camera/enabled` — leeg bestand om autostart aan te zetten.

## 3. Plaatsen en installeren

- SD-kaart als drager: zet `app.bin` op de SD-kaart als `g2h_app.bin`. Kopieer het
  op de camera één keer naar `/customer/app.bin` (of gebruik `g2h_place_appbin_live.py`).
- Herstart de camera. Bij de boot installeert `check_upgrade.sh` het pakket, ruimt
  alles op (intern + SD) en herstart opnieuw. Daarna draait onze service zodra
  identity en enabled aanwezig zijn.

Met `installatie/g2h_place_appbin_live.py`:
```bash
python3 installatie/g2h_place_appbin_live.py /tmp/g2h-appbin/app.bin --reboot
# een klaargezet bestand weer weghalen (annuleren):
python3 installatie/g2h_place_appbin_live.py x --remove
```

## 4. Terugdraaien

`/etc/init.d/S96g2hmattercamera disable` (en eventueel `/customer/g2h-matter-camera`
verwijderen). De vendor-camera, RTSP en HomeKit zijn nooit gewijzigd.
</content>
