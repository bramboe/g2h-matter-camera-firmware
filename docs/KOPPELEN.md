# Koppelen in Home Assistant

De camera koppelt als één Matter-apparaat. Je koppelt hem één keer; video,
snapshots, audio, terugpraten en bediening lopen daarna via die ene koppeling.

## Stappen

1. Open op de camera een koppelvenster (900 s) met het bijgeleverde
   installatiescript, of met `kill -USR1 <pid>` op het Matter-proces. Het venster
   verandert niets aan de bestaande koppelingen.
2. Zorg dat de camera met een koppelcode (setup code) beschikbaar is. Die code
   hoort bij jouw apparaat en staat niet in dit pakket; bewaar hem veilig.
3. In Home Assistant: **Instellingen → Apparaten en diensten → Apparaat
   toevoegen → Matter**, en voer de koppelcode in. Gebruik die code, niet een
   oude radio-QR.
4. Zodra Home Assistant het apparaat toont ("Aqara G2H Thread & Matter Camera"),
   is de koppeling klaar.

## Weergave in Home Assistant

Home Assistant toont Matter-camera's pas vanaf **HA 2026.11** met een
**Matter-server van schema 14**. Met een oudere HA verschijnt er nog geen
camerabeeld; de koppeling zelf blijft wel bestaan. Opnieuw koppelen is daarna
niet nodig.

## Privacy

Zet je de privacymodus aan (via de camera-entiteit in Matter), dan stopt de app
met beeld en geluid via Matter. RTSP en HomeKit blijven gewoon werken.
</content>
