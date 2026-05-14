# Vorgebackene Binaries

Pre-built ESP32-S3-Firmware-Images für den jeweils aktuellen RFNETHM-
Public-Release.  Beide Builds entstehen aus identischen Sourcen
(`firmware/` in dieser Branch); die Versionsnummer im Dateinamen
zeigt den Build-Stand an.

| Datei | Inhalt | Flash-Offset |
|---|---|---|
| `rfnethm-vMAJOR.MINOR.BUILD-factory.bin` | bootloader + partition-table + ota_data + Applikation, alles zusammengemerged | `0x0` |
| `rfnethm-vMAJOR.MINOR.BUILD-ota.bin`     | nur die Applikation (für OTA-Updates oder gezielten App-Re-Flash) | `0x10000` |

## Erst-Flash via USB (factory)

```sh
# ESP32-S3 in den Download-Mode versetzen:
#   BOOT-Button halten → RESET kurz → BOOT loslassen
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x0 rfnethm-v0.14.111-factory.bin
```

Pfad zur seriellen Schnittstelle ggf. anpassen:
- Linux: `/dev/ttyACM0`, `/dev/ttyUSB0` oder `/dev/serial/by-id/...`
- macOS: `/dev/cu.usbmodem*`
- Windows: `COM3` (o.ä.)

Nach dem Flash startet das Gerät neu und ist über die Console
erreichbar — `Improv-Serial` oder Captive-AP übernehmen das
WiFi-Provisioning.

## OTA-Update über die WebUI

Sobald das Gerät einmal im Netz ist, alle weiteren Updates ohne
USB-Verbindung:

```sh
curl -X POST --data-binary @rfnethm-v0.14.111-ota.bin \
    http://rfnethm-XXXX.local/api/ota
```

Alternativ über die WebUI (`http://rfnethm-XXXX.local/`) → *Stats* →
OTA-Form unten — Datei wählen → Upload.

## Gezielter App-Re-Flash via USB (ohne Partition-Tabelle anzufassen)

Wenn nur die Anwendung neu draufkommen soll (z.B. ein Build aus
einem anderen Branch testen) und Partition-Layout + NVS-Inhalt
erhalten bleiben sollen:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x10000 rfnethm-v0.14.111-ota.bin
```

## Voraussetzungen

- `esptool.py` ≥ 4.0 — kommt aus `pip install esptool` oder liegt
  PlatformIO bei (`~/.platformio/penv/bin/esptool.py`).
- ESP32-S3-Devkit mit nativem USB-OTG-PHY und Pin-Header für den
  HM-Modul-Slot.  Siehe README im Repo-Root für Hardware-Liste.

## Verifizieren

Die Build-Identität ist im Banner sichtbar — entweder seriell beim
Boot oder über die WebUI (Headbar zeigt `vMAJOR.MINOR.BUILD`).  Für
einen integrity-check der Bin-Files vor dem Flash:

```sh
sha256sum rfnethm-v0.14.111-*.bin
```
