# Ethernet-Anbindung — neuer Scope ggü. PIIF/CULFW32

Stand 2026-05-02. Diese Datei sammelt offene Fragen und nicht-verifizierte
Annahmen. Nichts hier ist final, alles ist Diskussions-Stand.

## Warum Ethernet zusätzlich zu WiFi

PIIF und CULFW32 setzen heute auf WiFi-STA als alleinigen Host-Pfad.
RFNETHM zielt auf den Anwendungsfall, in dem der Stick am Server-Schrank
oder zentralen Switch hängt, und für den Anwender ein RJ45 weniger
fragil ist als WiFi (kein Roaming, keine Power-Save-Latency, keine
Channel-Konflikte). WiFi bleibt parallel verfügbar, falls die finale
Position später geändert wird.

## ESP32-EMAC-Status — was geprüft werden muss

Konkret zu prüfen vor Hardware-Entscheidung — nicht ohne Datenblatt
festschreiben:

- ESP32-S2 / ESP32-S3 / ESP32-C6 / ESP32-H2: Nach aktuellem Wissensstand
  **kein interner EMAC** verfügbar; Ethernet nur über externen MAC+PHY
  am SPI-Bus. Vor finaler Festlegung im Espressif-Datenblatt verifizieren
  (z.B. via `mcp__espressif-documentation__search_espressif_sources`).
- ESP32 (Original) und ESP32-P4: haben internen EMAC mit RMII-Interface,
  bräuchten aber externen PHY (LAN8720 / RTL8201).

RFNETHM nutzt **ESP32-S3-WROOM-1-N16R2** — kein interner EMAC (verifiziert
Espressif-DS) → SPI-Ethernet ist der Pfad.

## SPI-Ethernet-Kandidaten

Vier Bausteine sind in ESP-IDF dokumentiert unterstützt — vor Auswahl
Datenblatt-Eckwerte und Verfügbarkeit prüfen, hier nur als Suchraster:

- **W5500** — TCP-Offload-Engine an Bord, Magnetics extern, sehr breit
  als Modul mit RJ45+Magjack verfügbar. Default-Vorschlag, weil ESP-IDF
  ein offizielles Beispiel mitliefert (`examples/ethernet/basic`).
- **W5100S** — älter, kleiner, sonst W5500-ähnlich.
- **KSZ8851SNL** — von Microchip, MAC+PHY in einem Chip, kein TCP-Offload,
  nutzt ESP-IDF-LWIP-Stack.
- **DM9051** — Davicom, ähnlicher Profil-Punkt wie KSZ8851.

Empfehlung Default: W5500-Modul mit Magnetic-Jack als erste Iteration,
weil als Steckmodul prototyp-tauglich. Endhardware kann das gleiche IC
on-board haben.

## SPI-Bus-Sharing

Auf dem ESP32-S3 ist **SPI2 (FSPI)** der User-SPI-Bus. Das HM-Modul
braucht keinen SPI (HM-MOD-RPI-PCB ist UART-only, Type-7-Frames laufen
über UART) → kein Konflikt, FSPI exklusiv für den W5500.

## W5500-Pinout (verbindlich, 2026-06-09)

**Überschreibt den „nichts final"-Disclaimer oben — dieser Pinout ist
festgelegt** und gilt **identisch für RFNETHM und das Schwesterprojekt
CDC2NET** auf der geteilten PCB.

SPI2/FSPI an den **IO-MUX-Direktpins** des ESP32-S3-WROOM-1-N16R2:

| W5500-Signal | ESP32-S3 GPIO | IO-MUX-Funktion | Richtung | Hinweis |
|---|---|---|---|---|
| SCLK  | `GPIO12` | FSPICLK  | MCU → W5500 | IO-MUX-direkt |
| MOSI  | `GPIO11` | FSPID    | MCU → W5500 | IO-MUX-direkt |
| MISO  | `GPIO13` | FSPIQ    | W5500 → MCU | IO-MUX-direkt |
| SCSn  | `GPIO10` | FSPICS0  | MCU → W5500 | HW-CS, aktiv-low |
| INTn  | `GPIO14` | (FSPIWP, ungenutzt) | W5500 → MCU | aktiv-low/open-drain → Pull-up, Falling-Edge-IRQ |
| RSTn  | `GPIO21` | —        | MCU → W5500 | aktiv-low, ≥500 µs Power-up-Pulse, danach Output-HIGH halten |

GPIO-Funktionen verifiziert gegen das ESP32-S3-WROOM-1-Datasheet
(Tab. 3-1, Stand 2026-06-09).

**Warum diese Pins:**

- **IO-MUX statt GPIO-Matrix.** GPIO10–14 sind die FSPI-Direktpins →
  umgehen die 2-Stufen-Matrix-Verzögerung. Matrix-SPI ist real auf
  ~40 MHz begrenzt, IO-MUX trägt bis 80 MHz. W5500 ist bis 80 MHz
  spezifiziert, auf Modulen real ~33–40 MHz robust. **Start bei 20 MHz**,
  dann hochtasten + per `ping`/Durchsatz validieren.
- **GPIO10–14 sind frei** und contiguous: nicht im HM-Block (`4–9, 15–18`),
  keine Strapping-Pins (`0/3/45/46`), kein in-package-Flash (`26–32`),
  nicht USB-OTG (`19/20`) / UART0 (`43/44`) → cleane Trace-Gruppe.
- **Bewusst NICHT GPIO35–37**: am N16R2 (Quad-PSRAM) zwar frei, aber am
  CDC2NET-Devkit YD-V1.4 (R8/Octal-PSRAM) vom PSRAM belegt. GPIO10–14
  sind auf **beiden** frei → identischer W5500-Code auf Devkit und PCB
  (geteilte-Plattform-Pflicht).
- **GPIO39–42 freigehalten** für einen optionalen JTAG-Header (S3 = Xtensa
  → JTAG, nicht SWD).
- Danach belegt für den TPS2065C-OTG-Switch (festgelegt 2026-06-09):
  **EN=`GPIO1`**
  (active-high, ext. Pull-down), **FLT/OC̄=`GPIO2`** (ext. Pull-up),
  **Status-LED=`GPIO38`**. Spare: `GPIO47`, `GPIO48`.

**ESP-IDF-Anbindung:** `SPI2_HOST` + `esp_eth` mit `eth_w5500`-Driver;
`eth_w5500_config_t.int_gpio_num = 14` (echter INT statt Polling →
niedrigere Latenz/CPU); RSTn (`GPIO21`) vor `esp_eth_driver_install`
manuell pulsen; SPI-Mode 0, `clock_speed_hz` initial 20 MHz.

## Power-Budget

- ESP32-S3 (WROOM-1): WiFi-TX-Peak ~300 mA.
- HM-MOD-RPI-PCB / RPI-RF-MOD: TX-Peak < 50 mA.
- W5500: typ. ~130 mA, peak ~150 mA bei 100BASE-TX-TX-Aktivität laut
  Wiznet-Datenblatt — Wert vor Layout im Datenblatt nachschlagen.
- Summe grob ≤ 500 mA → 5-V-USB-Eingang (USB-A oder USB-C) reicht
  bequem; LDO 3V3 muss ~600 mA leisten. RT9080-33 ist mit 300 mA zu
  klein. Kandidat: AP2114 / TLV757P / MIC5219 mit 600 mA-Klasse oder
  Buck-Converter (SY8089 etc.) für höhere Effizienz.
- **Optional PoE-PD** (z.B. Ag9700-Modul) als spätere Variante; Ethernet-
  Magjack mit PoE-Center-Tap pflicht.

## Host-Anbindung — beide NICs gleichzeitig?

ESP-IDF erlaubt mehrere `esp_netif`-Instanzen parallel, jede mit eigener
DHCP-Client und IP. UDP-Listener auf `0.0.0.0:3008` werden auf beiden
Interfaces gleichzeitig erreichbar, ohne explizites Routing.

Offene Frage: soll WiFi *automatisch* deaktiviert werden, sobald
Ethernet-Link hochkommt (energy-saving, weniger Funk-Stören anderer
Stations), oder bleibt WiFi parallel als Fallback aktiv? Default-Vorschlag
für erste Iteration: **WiFi parallel aktiv**, einfach weil das die
Improv-Provisioning-Story sauber hält.

## Fragen, die offen bleiben

- Soll der USB-Port ausschließlich Strom liefern, oder zusätzlich eine
  USB-Serial-Console über CP21x-Bridge? (Nicht den `hb_rf_usb_2`-Klon-Pfad,
  sondern als banaler Debug-Output.)
- Form: **entschieden 2026-06-09** → vertikaler USB-C-Stecker (OTG-Host,
  Busware-Sticks direkt) + USB-C-Buchse (CP2102N-Console/Power) +
  RJ45-Buchse; Box-Gehäuse.
- mDNS-Service-Type — `_raw-uart._udp` (HB-RF-ETH) bleibt; soll zusätzlich
  `_rfnethm._tcp` o.ä. mit Capabilities-Record exportiert werden?
- HmIP-RFUSB-formatkompatibles Gehäuse anstreben (Ergonomie) oder eigenes
  Gehäuse?
