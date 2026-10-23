# Installatie

De bewezen installatiemethode is via het netwerk (Telnet) vanaf een computer.
Een echte "op de SD-kaart zetten en de camera installeert zelf"-procedure is op
deze camera niet vastgesteld; zie onderaan.

## Benodigdheden

- De camera op hetzelfde netwerk, met Telnet bereikbaar en het camerawachtwoord.
- Python 3 op de computer.
- Deze map, met `g2h-matter-camera` en de map `installatie/`.

## Installeren

1. Controleer eerst de host (leest alleen, verandert niets):
   ```bash
   python3 installatie/g2h_host_check_live.py
   ```
2. Installeer de app. `/customer` heeft te weinig ruimte voor twee kopieën, dus
   gebruik `--in-place` (de vorige build in `installatie/builds/` dient als terugval):
   ```bash
   python3 installatie/g2h_camera_upgrade_live.py --in-place
   ```
   Verwacht `UPGRADED_APP_READY`. Een eenmalige
   `WARNING_VENDOR_CAMERA_RESTARTED` is normaal.
3. Herstellen na een mislukte installatie of om terug te zetten:
   ```bash
   python3 installatie/g2h_camera_service_recover_live.py
   ```

De scripts maken vóór elke flash een verse back-up, controleren hashes en zetten
bij een fout de vorige build terug. Ze raken de radio, de vendor-cameraconfiguratie
of het opstarten niet aan buiten het vervangen van de app op `/customer`.

## Koppelen

Zie `KOPPELEN.md`.

## Over de SD-kaart

Een bestand op de SD-kaart zetten is op deze camera géén bewezen installatieweg:
het opstarten van de camera leest geen installatiepakket van de SD-kaart. Een
SD-kaart-installatie zou aanpassingen aan het opstarten van de camera vergen, wat
risico op een onbruikbaar apparaat geeft. Daarom is de netwerkmethode hierboven
de aanbevolen, geteste reproductiemethode. Wil je toch een SD-kaart-route, bespreek
dat eerst; dat is apart onderzoek met terugvalplan.
</content>
