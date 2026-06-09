# Breadboard-Wiring: YD-ESP32-S3 V1.4 → eQ3 HM-MOD-RPI-PCB

Stand: 2026-05-07.  Ziel: HM-Modul am Breadboard ohne PCB-Spin testen, dabei
GPIO-Auswahl so treffen, dass spätere Übernahme aufs PCB-Design mit
**ESP32-S3-WROOM-1-N16R2** **ohne** Firmware-Pin-Define-Änderung läuft.

## Speicher-Eignung ESP32-S3-WROOM-1-N16R2

| Resource | N16R2 | RFNETHM-Bedarf | Verdict |
|---|---|---|---|
| Flash | 16 MB Quad SPI | RFNETHM ~976 KB; BER 12,7 MB LittleFS | comfortable |
| SRAM | 512 KB | ~300 KB DRAM-Heap nach IDF+Wi-Fi+lwIP | comfortable |
| PSRAM | 2 MB Quad | von keiner FW genutzt — Reserve | bleibt **AUS** (`# CONFIG_SPIRAM`) |
| ROM | 384 KB | Boot+Lib | n/a |

Production-Flash ist **16 MB** (= Devkit) → RFNETHMs bestehendes
`partitions_ota.csv` (2× 3 MB OTA + Storage) bleibt **unverändert**,
kein Neuschnitt. Jede der drei Firmwares flasht ihr eigenes Layout:
RFNETHM/CDC2NET = Dual-OTA, BER = single-app + 12,7 MB LittleFS.

## eQ3-Modul-Pinout (40-pin-Header, RPi-HAT-kompatibel)

**Korrigiert 2026-05-07** nach Sichtung des ELV-Original-Schaltplans
und User-Verifikation am RPi-RF-MOD-Pinout.

| eQ3-Pin | RPi-BCM | Signal | Richtung | Welches Modul |
|---|---|---|---|---|
| 1 | 3V3 | Versorgung 3,3 V | — | **HM-MOD-RPI-PCB** (nutzt nur 3V3) |
| 2, 4 | 5V | Versorgung 5 V | — | **RPI-RF-MOD** (hat eigenen LDO, kein 3V3-Pfad!) |
| 6, 9, 14, 20, 25, 30, 34, 39 | GND | Masse | — | beide |
| 8 | BCM14/TXD | **HM_RX** (Modul-Eingang) | MCU TX → Modul | beide |
| 10 | BCM15/RXD | **HM_TX** (Modul-Ausgang) | Modul → MCU RX | beide |
| 11 | BCM17 | C2D-Debug-Daten (TRX1.3 P2.7) | bidir | HM-MOD-RPI-PCB |
| 12 | BCM18 | **HM_RST active-LOW** + C2CK | MCU → Modul | HM-MOD-RPI-PCB (bei RPI-RF-MOD nicht beschaltet) |
| 32 | BCM12 | **HM_BTN** | MCU → Modul | RPI-RF-MOD |
| 35 | BCM19 | **HM_RST active-HIGH** (alt-Reset) | MCU → Modul | RPI-RF-MOD |
| 36 | BCM16 | **HM_RED** (LED) | Modul → MCU | RPI-RF-MOD |
| 38 | BCM20 | **HM_GREEN** (LED) | Modul → MCU | RPI-RF-MOD |
| 40 | BCM21 | **HM_BLUE** (LED) | Modul → MCU | RPI-RF-MOD |

**Reset-Polaritäten — empirisch verifiziert:**

| Modul | RST-Pin | Polarität | Verifikation |
|---|---|---|---|
| HM-MOD-RPI-PCB | eQ3-12 / GPIO16 | **active-LOW** (drive 0 → reset, 1 → run) | piVCCU/generic_raw_uart.c + Tag-Hit 2026-05-07 |
| RPI-RF-MOD | eQ3-35 / GPIO7 | **active-HIGH** (drive 1 → reset, 0 → run) | empirisch durch source_uart-Discovery 2026-05-08 |

Beide Module haben **keinen externen Pull-Up** auf der RST-Linie — die
ESP-Side muss nach dem Pulse entweder Output halten oder Input mit
internem Pull (45 kΩ) auf den Inactive-Pegel binden, sonst floatet die
Linie zurück in den Reset.

**Wichtige Korrekturen** gegenüber älterer Tabelle:
- Pin 11 ↔ 12 vertauscht (Pin 12 ist RST, nicht 11)
- HM_RST_INV liegt auf **Pin 35** (= BCM19), nicht 13
- HM_BTN liegt auf **Pin 32** (= BCM12), nicht 12
- LED-Reihenfolge war BLUE/GREEN getauscht — korrekt:
  RED=Pin 36, GREEN=Pin 38, BLUE=Pin 40

Die zwei Module unterscheiden sich:
- **HM-MOD-RPI-PCB** (BidCoS-only, ELV-Modul) — nutzt nur Pin 1 (3V3),
  8, 10, 11, 12 + GND.  RST = Pin 12.  Keine LEDs, kein BTN am Header.
- **RPI-RF-MOD** (HmIP+BidCoS dual, A.R.s Folge-Hardware) — wird **mit
  5 V auf Pin 2/4** versorgt (eigener on-board-LDO macht intern die
  3,3 V für EFM32 + CC1101).  Pin 1 (3V3) ist auf diesem Modul *nicht*
  angeschlossen — wer 3V3 statt 5V auf den Header gibt, hat ein stummes
  Modul.  Zusätzlich genutzt: Pin 32 (BTN), 35 (alt-Reset),
  36/38/40 (RGB-LED).  Reset-Linie ist Pin 35 (siehe
  piVCCU/generic_raw_uart.c `use_alt_reset_pin`-Flag), Pin 12 wird hier
  nicht für Reset verwendet.

## ESP32-S3-Pin-Mapping (YD V1.4 + N8 future-proof)

GPIO-Auswahl beachtet:
- **Strapping-Pins** (`GPIO0, 3, 45, 46`) → vermieden
- **USB-OTG D±** (`GPIO19, 20`) → vermieden (auf YD V1.4 USB2-Connector)
- **SPI0/1 in-package-Flash** (`GPIO26–32`) → vermieden
- **UART0 Console** (`GPIO43, 44`) → vermieden
- **WS2812-RGB-LED** (`GPIO48`) auf YD V1.4 → vermieden
- **GPIO35–37** am Production-WROOM-1-N16R2 (Quad-PSRAM) frei, am YD V1.4
  (WROOM-1 mit R8/Octal-PSRAM) vom PSRAM belegt → vermieden für portable
  Wahl. **GPIO33/34** am WROOM-1 nicht herausgeführt.

| ESP32-S3 GPIO | YD V1.4 Pin | eQ3-Pin | Signal | UART/Function |
|---|---|---|---|---|
| `GPIO17` | J1/10 | 8 | HM_RX | UART1 TX |
| `GPIO18` | J1/11 | 10 | HM_TX | UART1 RX |
| `GPIO16` | J1/9 | **12** | **HM_RST** (HM-MOD-RPI-PCB) | GPIO out, active-low |
| `GPIO7`  | J1/7 | **35** | **HM_RST_INV** (RPI-RF-MOD alt-Reset) | GPIO out, active-low |
| `GPIO15` | J1/8 | **32** | HM_BTN | GPIO out (push) |
| `GPIO4`  | J1/4 | **36** | HM_RED | GPIO in (LED-readback) |
| `GPIO5`  | J1/5 | **38** | HM_GREEN | GPIO in |
| `GPIO6`  | J1/6 | **40** | HM_BLUE | GPIO in |
| `3V3` | J1/1 | 1 | Versorgung | YD-LDO CJ6107A33 ≥ 600 mA |
| `GND` | J1/22 | 6, 9, … | GND-Stern | mehrere Punkte |

C2-Debug-Pin 11 (BCM17) bewusst **nicht** verbinden — am HM-MOD-RPI-PCB
ist das die SiLabs-C2-Daten-Linie, würde von einem MCU-Output gestört.

YD V1.4 J1 (linke Stiftleiste, 22-pin) — relevante Pins von oben gezählt:
1=3V3, 2=GND, 3=CHIP_PU, 4=GPIO4, 5=GPIO5, 6=GPIO6, 7=GPIO7, 8=GPIO15,
9=GPIO16, 10=GPIO17, 11=GPIO18, 12=GPIO8, 13=GPIO3, 14=GPIO46, 15=GPIO9,
16=GPIO10, 17=GPIO11, …, 22=GND.

(Falls J1/17 für SCL belegt anders als hier — vor Crimpen mit Multimeter
durchklingeln, der V1.4-Schaltplan ist die Wahrheit.)

## RPi-Expansionsheader-Adapter

Pragmatischer Aufbau: 40-pin RPi-Female-Header über Steckdraht-Brücken
auf das Breadboard, dort an die 22-pin J1-Buchse des YD-Devkits.  Modul
sitzt im RPi-Header wie auf einer Pi.

Mindest-Brücken zum Modul (Power abhängig vom Modul-Typ):

```
YD-Devkit                eQ3-Header
─────────                ──────────
3V3   (J1/1)        ───► Pin 1  (3V3)    [nur HM-MOD-RPI-PCB]
5V    (VBUS / J3 5V)───► Pin 2  (5V)     [nur RPI-RF-MOD — Pflicht!]
GND   (J1/22)       ───► Pin 6  (GND) + Stern auf 9, 14, 20, 25, 30, 34, 39
GPIO17 (J1/10)      ───► Pin 8  (HM_RX)
GPIO18 (J1/11)      ───► Pin 10 (HM_TX)
GPIO16 (J1/9)       ───► Pin 12 (HM_RST,    HM-MOD-RPI-PCB, active-LOW)
GPIO7  (J1/7)       ───► Pin 35 (HM_RST_INV, RPI-RF-MOD,    active-HIGH)
```

Die 5 V kommen am YD-ESP32-S3-COREBOARD V1.4 direkt aus der USB-C-
Power-Buchse (an einigen Pads / am 5V-Pin der Stiftleiste abgreifbar
— vor Crimpen mit Multimeter gegen den USB-Power-Eingang durchklingeln).
Aus 5 V wird am RPI-RF-MOD intern via on-board-LDO die 3,3 V für
EFM32+CC1101 erzeugt; deshalb braucht das Modul keine 3V3-Leitung
und ist auch nicht damit zu versorgen.

Optional, falls RPI-RF-MOD-Tests mit LEDs+BTN anstehen:

```
J1/8  GPIO15 ─► Pin 32 (HM_BTN)
J1/4  GPIO4  ─► Pin 36 (HM_RED)
J1/5  GPIO5  ─► Pin 38 (HM_GREEN)
J1/6  GPIO6  ─► Pin 40 (HM_BLUE)
```

C2-Debug-Pin 11 (BCM17) bewusst **nicht** verbinden — am HM-MOD-RPI-PCB
würde ein MCU-Output dort die SiLabs-C2-Debug-Daten-Linie stören.

## Versorgungs-Hygiene

- **HM-MOD-RPI-PCB**: 3,3 V vom YD-Devkit (J1/1, CJ6107A33-LDO am
  Devkit) auf eQ3-Pin 1.  TX-Burst zieht <50 mA, der LDO hat ausreichend
  Reserve.
- **RPI-RF-MOD**: **5 V Pflicht** (USB-Power-Schiene des YD) auf
  eQ3-Pin 2 (oder Pin 4).  Das Modul hat einen eigenen on-board-LDO
  und schaltet daraus seine internen 3,3 V.  Eine 3V3-Speisung am
  Header-Pin 1 reicht **nicht** und wird auch nicht intern weiter-
  geleitet — das Modul bleibt schlicht stumm.  Wer beide Module
  abwechselnd am gleichen Header benutzt, kann beide Pfade parallel
  verkabeln (Pin 1 = 3V3, Pin 2/4 = 5V); die Module belegen jeweils
  nur ihren eigenen.
- **GND-Stern**: das Modul hat 8 GND-Pins am Header — mind. zwei davon
  zum YD durchziehen, Rückleiter direkt vom HM-RX-/HM-TX-Pin-Cluster.
- **Antennen-Massnahme**: auf dem Breadboard ist die HF-Antenne an
  Drahtbruch-Effekten anfällig — IPEX auf Stub-Antenne, Stub gerade
  nach oben, kein metallisches Material in 5 cm Radius.

## PCB-Übernahme (ESP32-S3-WROOM-1-N16R2)

**Pin-Defines bleiben unverändert.**  Alle gewählten GPIOs sind am
WROOM-1-N16R2 herausgeführt und frei:
- `GPIO4–9, 15–18`: am WROOM-1 gebondet, kein PSRAM-Konflikt (R2=Quad)
- Strapping-Pins werden weiterhin nicht angefasst
- USB-OTG-Pins (`GPIO19, 20`) bleiben für USB-Connector reserviert

Auf dem PCB-Design dann zusätzlich:
- VBUS-GPIO-Switch (für Stick-Power-Cycle)
- Console-UART-Header
- SWD-Header

## Verifizierung dieser Wahl

- ESP32-S3-WROOM-1-N16R2: 16 MB Flash, 512 KB SRAM, 2 MB Quad-PSRAM (aus),
  36 herausgeführte GPIO, PCB-Antenne, −40…85 °C — verifiziert gegen das
  ESP32-S3-WROOM-1-Datasheet (Tab. 1-1)
- GPIO-Restriktionen: ESP32-S3 Datasheet §2.3.4 + ESP-IDF GPIO API-Doku
- HM-MOD-RPI-PCB-Pinout: HB-RF-ETH-Referenz + RPi-40-pin-BCM-Standard

## Status

Plan erstellt 2026-05-07.  Hardware-Verifikation (Wackelkontakt-Check,
HM_TX-Idle-Pegel, UART-Echo) folgt sobald die Steckdraht-Brücken
gezogen sind.
