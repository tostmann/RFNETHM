# RFNETHM — HmIP-Funk übers Netzwerk

<p align="center">
  <img src="logos/Lab-Setup.jpeg" alt="DIY WiFi Gateway: HmIP-RFUSB-Stick am ESP32-S3-Devkit" width="420">
</p>

**Ein Adapter, der einen unmodifizierten HmIP-RFUSB-Stick (oder ein
RPI-RF-MOD / HM-MOD-RPI-PCB am Pin-Header) ins Netzwerk hängt** — statt
ihn an einen lokalen USB-Port zu zwingen.  Damit wird der Funk-Stick
zu einem netzwerkweit erreichbaren BidCoS-/HmIP-Frontend für FHEM,
Homegear, piVCCU oder RaspberryMatic — ohne dass eine echte CCU oder
ein Pi am Funk-Standort stehen muss.

Status: **Firmware v0.15.0** (2026-05-25), Devkit-Bringup abgeschlossen,
alle vier Kreuze der Ground-Truth-Matrix (HB-RF-ETH × {HM-MOD,
RPI-RF-MOD} und RFNETHM × {HM-MOD, RPI-RF-MOD}) live ge-flasht. Multi-Client-
Stress-Test (4 HMU + 3 raw-UART + UDP + HTTP-Poll @5 Hz parallel) über
mehrere Minuten ohne HTTP-Aussetzer, ohne Reconnect-Storm, Uptime
ungebrochen. v0.15.0 bringt OTA-Update-Badge (WebUI zeigt automatisch
wenn auf [`install.busware.de/rfnethm/`](https://install.busware.de/rfnethm/)
eine neuere Firmware verfügbar ist) und einen vergrößerten LWIP-Socket-Pool
gegen Reconnect-Storms. Eigenes PCB ist in Planung.

Flashen geht am bequemsten über den **Browser-Webflasher** unter
[`https://install.busware.de/rfnethm/`](https://install.busware.de/rfnethm/) —
ESP32-S3 anstecken, *Connect*, fertig.  Updates kommen danach OTA übers
WebUI; das Gerät meldet selbst wenn eine neue Version online ist.

---

## Datenfluss

```
   HmIP-RFUSB           RFNETHM                dein Server
   (eq-3 Original)      (ESP32 + WLAN/Eth)     (FHEM, Homegear,
                                                piVCCU, RaspberryMatic)
       USB ───────────► USB-Host
                        │
                        └─── WLAN/Ethernet ───►  hb_rf_eth.ko
                                                 /dev/raw-uart
                                                 oder TCP-Socket
```

Alternativ statt USB-Stick: **RPI-RF-MOD** oder **HM-MOD-RPI-PCB**
direkt auf den 40-Pin-Header — selber Adapter, gleicher Server-Pfad.

---

## Wozu das gut ist

- **Funkstandort frei wählbar.** Stick steht da, wo der Empfang gut ist
  (Estrich, Garage, Pegel-Sweet-Spot); die Hausautomation läuft woanders.
- **Drop-in für alle gängigen Stacks.** Aus Sicht des Servers ist es
  ein normales `hb_rf_eth`-Gerät (Alex Reinerts Kernel-Modul) **oder**
  eine HM-MOD-RPI-PCB-Bridge (TCP/2330). Beide Wege gleichzeitig nutzbar.
- **Kein zusätzlicher Pi nötig.** Wer bisher einen Pi nur als Funkträger
  nutzt, kann ihn ersetzen.
- **Kein Custom-RF-Stack.** Der Funk-Teil bleibt der echte eq-3-Stick mit
  eq-3-Firmware — alle BidCoS-/HmIP-Eigenheiten, AES, Pegelhandling
  sind genauso wie im Original.

---

## Netzwerk-Schnittstellen (parallel offen)

| Port | Format | Was redet damit |
|---|---|---|
| **UDP 3008** | HB-RF-ETH wire-format | `hb_rf_eth.ko` → `/dev/raw-uart` für piVCCU / RaspberryMatic |
| **TCP 2330** | HMUARTLGW | FHEM `CUL_HM`, Homegear, jeder Stack der eine HM-MOD-RPI-PCB-Bridge erwartet |
| **TCP 2329** | Raw-Bytestream | bmcond, eigene Tools, Reverse-Engineering |
| **HTTP 80** | WebUI + REST | Status, OTA-Update, WLAN-Setup, Live-Log |
| **mDNS** | `_raw-uart._udp:3008` | Auto-Discovery (`rfnethm-XXXX.local` + Alias `rfnethm.local`) |

Eingehende Funk-Frames werden gleichzeitig an alle verbundenen Clients
gespiegelt.  Senden ist mit einem **TX-Master-Soft-Lock** gegen
Mehrfach-Schreiber abgesichert (erster Sender bekommt den Stick für
5 s, danach übernimmt der nächste; per WebUI festpinnbar).

---

## Was Du brauchst

- **Funk-Hardware** (eine Variante):
  - HmIP-RFUSB-Stick (eq-3 Original, USB `1B1F:C020`), oder
  - RPI-RF-MOD am 40-Pin-Header — **braucht 5 V auf Header-Pin 2/4**
    (eigener on-board-LDO, kein 3V3-Pfad), oder
  - HM-MOD-RPI-PCB am 40-Pin-Header — 3,3 V auf Pin 1 reicht
- **ESP32-S3-Devkit** mit nativem USB-OTG-PHY (z.B. YD-ESP32-S3 V1.4)
  und Pin-Header für den HM-Modul-Slot.  Ein eigenes PCB (ESP32-S3-WROOM-1-N16R2
  + W5500 + vertikaler USB-C-OTG-Stecker + HM-Slot) ist in Vorbereitung.
- **5 V / 200 mA** Versorgung über die zweite USB-C-Buchse am Devkit.
  Wer RPI-RF-MOD nutzt, muss zusätzlich 5 V auf den HM-Header (Pin 2/4)
  durchziehen — siehe [`docs/breadboard_wiring.md`](docs/breadboard_wiring.md).
- **WLAN** im Lab — Ethernet kommt mit dem eigenen PCB-Spin.

---

## Inbetriebnahme (Kurz)

1. **Flashen.** Drei Wege, von "einfach" nach "Source-Build":

   - **Webflasher (empfohlen)** — Browser auf
     [`https://install.busware.de/rfnethm/`](https://install.busware.de/rfnethm/)
     öffnen, ESP32-S3 per USB anstecken, **Connect** klicken. Funktioniert
     auf Chrome / Edge / Opera (Web-Serial-API).  Kein lokaler Build,
     keine PlatformIO-Installation nötig.
   - **CLI mit fertigem Image** — siehe
     [Vorgebackene Binaries](#vorgebackene-binaries-flash-via-esptool) unten.
   - **Source-Build via PlatformIO**:
     ```sh
     pio run -e rfnethm -t upload
     ```
2. **WLAN provisionieren.** Zwei Wege:
   - **Improv-Serial**: Browser → improv-wifi.com → ESP über die
     Console-USB-Buchse verbinden → Credentials eingeben.
   - **Captive-AP-Fallback**: erscheint nach 30 s als WLAN
     `RFNetHM XXXX`, Handy verbindet sich, jede HTTP-Anfrage landet
     im Setup-Formular.
3. **Im WebUI** (`http://rfnethm.local` oder `http://rfnethm-XXXX.local`) Status checken; alle
   Funk-Frames stehen sofort am Netzwerk-Port bereit.
4. **Server-Seite** konfigurieren:
   - piVCCU / RaspberryMatic: `hb_rf_eth.ko` mit der IP des RFNETHM laden
     (Schritt-Screenshot weiter unten unter
     [RaspberryMatic / OpenCCU einbinden](#raspberrymatic--openccu-einbinden)).
   - FHEM: `define hmusb HMUARTLGW rfnethm-XXXX.local:2330`.

---

## Vorgebackene Binaries (Flash via esptool)

Für CLI-Nutzer, die keinen Web-Serial-fähigen Browser haben oder den
Flash skripten wollen, liegen unter [`webflasher/`](webflasher/) im
Repo dieselben Images, die auch der Webflasher serviert:

- **`webflasher/factory_rfnethm_esp32s3.bin`** — Komplett-Image
  (bootloader + partition-table + ota_data + Applikation).  Erst-Flash
  für ein jungfräuliches oder anders belegtes ESP32-S3.
- **`webflasher/firmware_rfnethm_esp32s3.bin`** — nur die Applikation
  (offset `0x10000`).  Für OTA-Updates über die WebUI, oder als
  gezielter Re-Flash auf bereits provisioniertes Gerät.

Image-Inhalt ist deckungsgleich mit dem aktuellen Release-Tag und der
Version, die der Webflasher unter
[`https://install.busware.de/rfnethm/`](https://install.busware.de/rfnethm/)
ausliefert (siehe `webflasher/manifest.json`).

```sh
# 1) Erst-Flash via USB (ESP32-S3 im Download-Mode — BOOT-Button halten,
#    RESET kurz, BOOT loslassen).  Port-Pfad gegebenenfalls anpassen.
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x0 webflasher/factory_rfnethm_esp32s3.bin

# 2) OTA-Update via WebUI (empfohlen sobald das Gerät im Netz ist —
#    kein Druck auf BOOT-Button nötig, keine USB-Verbindung):
curl -X POST --data-binary @webflasher/firmware_rfnethm_esp32s3.bin \
    http://rfnethm-XXXX.local/api/ota

# 3) Reiner Re-Flash der Anwendung über USB (ohne Partition-Tabelle
#    anzufassen — z.B. wenn nur ein neuer Build aufgespielt werden soll):
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x10000 webflasher/firmware_rfnethm_esp32s3.bin
```

`esptool.py` liegt bei PlatformIO bei (`~/.platformio/penv/bin/esptool.py`)
oder kommt out-of-the-box aus `pip install esptool`.  Auf macOS / Windows
ist der Port-Pfad entsprechend `/dev/cu.usbmodem*` bzw. `COMx`.

---

## WebUI

Die eingebaute Web-Oberfläche ist unter `http://rfnethm.local/`
erreichbar (kurzer Alias, gedacht für den Normalfall mit einem Stick im
Netz) bzw. unter dem eindeutigen `http://rfnethm-XXXX.local/` (XXXX =
letzte 4 Hex-Stellen der MAC, stabiler Pfad).  Sie zeigt
Sources, Sinks und System-Status als Kachel-Dashboard — inklusive
Reset-Reason, Heap- und Stack-HWM-Indikator für den Dauerlauf-Betrieb,
TX-Master-Soft-Lock pro Sink, Live-Log, OTA-Update und WLAN-Provisioning.

> Falls mehrere RFNETHM-Sticks am gleichen LAN hängen: `rfnethm.local`
> wird von beiden gleichzeitig beworben — welcher tatsächlich antwortet,
> hängt vom mDNS-Race ab.  Wer eindeutig ansprechen will, nimmt
> `rfnethm-XXXX.local`.
Light- und Dark-Theme sind per Toggle in der Headbar umschaltbar, die
Wahl wird pro Browser persistiert; ohne explizite Wahl folgt das
Theme der OS-Präferenz (`prefers-color-scheme`).

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="logos/webUIDark.png">
    <img alt="RFNETHM WebUI (Light/Dark — folgt der OS-Präferenz beim Anzeigen auf GitHub)" src="logos/webUI.png" width="460">
  </picture>
</p>

---

## RaspberryMatic / OpenCCU einbinden

In RaspberryMatic bzw. OpenCCU unter **System-Optionen → Erweiterte
Einstellungen** trägst Du die IP-Adresse (oder den mDNS-Namen
`rfnethm-XXXX.local`) des RFNETHM in das Feld
**„IP-Adresse (HB-RF-ETH)"** ein und bestätigst mit *Änderungen
speichern*.  Danach lädt RaspberryMatic den `hb_rf_eth.ko`-Pfad neu
und der angeschlossene HmIP-RFUSB-Stick (oder das RPI-RF-MOD am
Pin-Header) erscheint als lokales `/dev/raw-uart`.

<p align="center">
  <img src="logos/CCU-Config.png" alt="OpenCCU / RaspberryMatic — IP-Adresse (HB-RF-ETH) eintragen" width="640">
</p>

---

## Status — was funktioniert

**Verifiziert (live gegen reale Hardware):**

- USB-Host-Init des HmIP-RFUSB-Stick, Mode-Switch BL → App
- Beide HM-Modul-Familien am Pin-Header (HM-MOD-RPI-PCB / Co_CPU
  und RPI-RF-MOD / DualCoPro), Auto-Erkennung beim Boot
- HMUARTLGW-Codec byte-genau gegen Live-Captures
- `hb_rf_eth.ko`-Pfad bis zur Aktor-Toggle-Schaltung in RaspberryMatic
- FHEM `CUL_HM` / HMUARTLGW-Bridge bis Phase D (TX, RX, AES-Key-Storage)
- Coprocessor-Firmware-Flash von beiden Modul-Familien über bmcond
  via UDP-Transport (RPI-RF-MOD 4.4.22 ⇆ 4.2.14, HM-MOD 2.8.6 ⇆ 2.2.1)
- Multi-Client-Fanout, TX-Master-Lock, mDNS, Captive-AP-Fallback, OTA

**Nicht fertig:**

- AES-Authentifizierung in der Legacy-Bridge (Phase E — mehrtägig,
  kein Blocker für AES-aware Devices).
- Eigenes PCB (Schaltplan und Pin-Mapping stehen; siehe
  `docs/decisions.md`).

---

## Was RFNETHM **nicht** ist

- **Kein** Drop-in-Replacement für den HmIP-RFUSB am unmodifizierten
  RaspberryMatic-Kernel-Treiber: der USB-Pfad ist via ECDSA gesperrt
  (bewusste Design-Entscheidung von Alex Reinert, wird respektiert).
  Variante B des Projekts (CP2102N mit OTP-Brand `1B1F:C020`) ist eine
  separate Bauform und ohne ESP32.
- **Kein** eigenes Wireformat.  UDP/3008 mit HB-RF-ETH-Frames und
  TCP/2330 mit HMUARTLGW sind die Default-Sprachen.
- **Keine** BidCoS-/HmIP-Protokoll-Interpretation in der Stick-Firmware
  — reiner Transport, alle HM-Logik bleibt downstream.

---

## Ist das was für mich?

- **Ja**, wenn Du HmIP-Funk in einer Linux-Hausautomation (FHEM,
  Homegear, piVCCU, RaspberryMatic) nutzt und Funk-Standort vom
  Server-Standort entkoppeln willst.
- **Ja**, wenn Du einen alten Pi loswerden willst der nur den
  HmIP-Stick / das RPI-RF-MOD trägt.
- **Nein**, wenn Du eine vollständige CCU-Funktionalität ohne
  Server-Stack willst — dafür ist RaspberryMatic die richtige Adresse,
  RFNETHM ist nur der Funk-Adapter.
- **Nein**, wenn Du Klassik-Homematic-Funk (BidCoS, AskSinPP) ohne
  HmIP brauchst — da ist ein CUL/COC der bessere Match.

---

## Mehr lesen

- [`docs/intro.md`](docs/intro.md) — ausführliche Einführung
- [`docs/breadboard_wiring.md`](docs/breadboard_wiring.md) — Verkabelung
  am Devkit
- [`docs/ethernet_addition.md`](docs/ethernet_addition.md) — Ethernet-
  Anbindung für den PCB-Spin
- [`docs/diagrams/`](docs/diagrams/) — Architektur- und
  Message-Flow-Diagramme

---

## Lizenz

GPL-2.0-or-later (analog PIIF / CULFW32).  Listener-Code ist eigene
Reimplementierung gegen die HB-RF-ETH-Spec — kein Copy aus dem
CC-BY-NC-SA-4.0-lizenzierten Original-Repo.

---

<p align="center">
  <img src="logos/busmatic.png" alt="busmatic" width="160">
</p>

<p align="center"><sub>RFNETHM ist ein Lab-Projekt aus dem Tostmann-RF-Lab.</sub></p>
