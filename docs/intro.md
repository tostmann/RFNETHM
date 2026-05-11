# RFNETHM — was ist das?

<p align="center">
  <img src="../logos/Lab-Setup.jpeg" alt="DIY WiFi Gateway: HmIP-RFUSB-Stick am ESP32-S3-Devkit" width="380">
</p>

Ein Adapter, der einen **unmodifizierten HmIP-RFUSB-Stick** ins Netzwerk
hängt, statt ihn an einen lokalen USB-Port zu zwingen.  Damit wird der
Stick zu einem netzwerkweit erreichbaren BidCoS-/HmIP-Funk-Frontend
für FHEM, Homegear, piVCCU oder RaspberryMatic — ohne dass eine echte
CCU oder ein Pi am Funk-Standort stehen muss.

Datenfluss:

```
   HmIP-RFUSB        RFNETHM-Stick           dein Server
   (eq-3 Original)   (ESP32 + WLAN/Eth)      (FHEM, Homegear,
                                              piVCCU, RaspberryMatic)
       USB ───────────► USB-Host
                        │
                        └── WLAN/Ethernet ────────►  hb_rf_eth.ko
                                                     /dev/raw-uart
                                                     oder TCP-Socket
```

## Wozu das gut ist

- **Funkstandort frei wählbar.**  Stick steht da, wo der Empfang gut
  ist (Estrich, Garage, Pegel-Sweet-Spot), die Hausautomation läuft
  woanders.
- **Drop-in für alle gängigen Stacks.**  Aus Sicht des Servers ist es
  ein normales `hb_rf_eth`-Gerät (Kernel-Modul von Alexander Reinert)
  oder ein HM-MOD-RPI-PCB-Bridge (TCP-Socket auf Port 2330).  Beide
  Wege gleichzeitig nutzbar.
- **Kein zusätzlicher Pi nötig.**  Wer bisher einen Raspberry Pi nur
  als HmIP-Funkträger nutzt, kann den ersetzen.
- **Kein Custom-RF-Stack.**  Der Funk-Teil ist und bleibt der echte
  eq-3-Stick mit eq-3-Firmware — alle BidCoS-/HmIP-Eigenheiten,
  AES-Stack, Pegelhandling sind genauso wie im Original.

## Was unterm Deckel passiert

Der ESP32 ist USB-Host für den HmIP-RFUSB-Stick.  Er übersetzt
zwischen dem stick-internen HMUARTLGW-Protokoll und drei Netzwerk-
Schnittstellen, die parallel offen sind:

| Port | Format | Was redet damit |
|---|---|---|
| **UDP 3008** | HB-RF-ETH wire-format | Linux-Kernel-Modul `hb_rf_eth.ko` → erscheint als `/dev/raw-uart` für piVCCU/RaspberryMatic |
| **TCP 2330** | HMUARTLGW | FHEM `CUL_HM`, Homegear, jeder Stack der auf einen HM-MOD-RPI-PCB-Bridge wartet |
| **TCP 2329** | Raw-Bytestream | bmcond, eigene Tools, Reverse-Engineering |

Eingehende Funk-Frames werden gleichzeitig an alle verbundenen
Clients gespiegelt.  Senden ist mit einem **TX-Master-Lock** gegen
Mehrfach-Schreiber abgesichert (sonst wirft der eq-3-Stick mit nur
einem 8-bit-Sequenzzähler bei zwei gleichzeitigen Sendern Frames
weg).  Default: erster Sender bekommt für 5 s den Stick, danach
übernimmt wer als nächstes sendet — über das WebUI auf einen
bestimmten Pfad festpinnbar.

Konfiguration läuft über ein eingebautes WebUI im busware-Stil:
WLAN-Provisioning (Improv-Serial oder Captive-AP-Fallback „RFNetHM
XXXX" auf dem Handy), Status-Kacheln pro Schnittstelle, OTA-Update,
Live-Console.

## Status (Mai 2026, Firmware v0.14.55)

**Funktioniert verifiziert:**

- USB-Host-Init des HmIP-RFUSB-Stick, Mode-Switch Bootloader → App
- HMUARTLGW-Codec byte-genau gegen Live-Captures
- `hb_rf_eth.ko`-Pfad bis zur Aktor-Toggle-Schaltung in
  RaspberryMatic
- FHEM-`CUL_HM`-Bridge bis Phase D (TX, RX, AES-Key-Storage)
- Multi-Client-Fanout, TX-Master-Soft-Lock, mDNS-Discovery,
  Captive-AP-Fallback, OTA

**Nicht fertig:**

- Echte AES-Authentifizierung in der Legacy-Bridge (Phase E,
  mehrtägig, kein Blocker für AES-aware Devices)
- Eigenes PCB — aktuell laufen alle Tests am ESP32-S3-Devkit
  (YD-ESP32-S3 V1.4) mit gestecktem Stick

## Was Du brauchst

- **HmIP-RFUSB-Stick** (eq-3 Original, USB-VID:PID `1B1F:C020`)
- **ESP32-S3-Devkit** mit nativem USB-OTG-PHY und VBUS-Switch oder
  Permanent-VBUS am OTG-Port (z.B. YD-ESP32-S3 V1.4 oder vergleichbar)
- Ethernet-Module ist optional — für Bring-up reicht WLAN, eigenes
  PCB bekommt zusätzlich W5500
- 5 V / 200 mA Versorgung

Die eigene PCB-Variante (ESP32-S3-MINI-1-N8 + W5500 + USB-A für den
Stick) ist geplant; bis dahin läuft alles am ESP32-S3-Devkit, siehe
`docs/breadboard_wiring.md` für die Verkabelung.

## Mehr lesen

- `README.md` — Konzept-Überblick
- `docs/breadboard_wiring.md` — Verkabelung am Devkit
- `docs/ethernet_addition.md` — Ethernet-Anbindung (PCB-Spin)
- `docs/diagrams/` — Architektur- und Message-Flow-Diagramme

## Ist das was für mich?

- **Ja**, wenn Du HmIP-Funk in einer Linux-Hausautomation (FHEM,
  Homegear, piVCCU, RaspberryMatic) nutzt und den Funk-Standort vom
  Server-Standort entkoppeln willst.
- **Ja**, wenn Du einen alten Pi loswerden willst der nur den
  HmIP-Stick trägt.
- **Nein**, wenn Du eine vollständige eq-3-CCU-Funktionalität ohne
  Server-Stack willst — dafür ist RaspberryMatic die richtige
  Adresse, RFNETHM ist nur der Funk-Adapter.
- **Nein**, wenn Du Klassik-Homematic-Funk (BidCoS, AskSinPP) ohne
  HmIP brauchst — der HmIP-RFUSB-Stick *kann* zwar BidCoS empfangen
  und senden, ist aber nicht das natürliche Frontend dafür.  Da ist
  ein CUL/COC oder HM-MOD-RPI-PCB der bessere Match.

---

<p align="center">
  <img src="../logos/busmatic.png" alt="busmatic" width="180">
</p>

<p align="center"><sub>RFNETHM ist ein Lab-Projekt aus dem Tostmann-RF-Lab. Open Source, GPL-2.0-or-later.</sub></p>
