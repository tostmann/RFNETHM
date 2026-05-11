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

Solange RFNETHM bei ESP32-C6-MINI-1 bleibt (Konsistenz mit PIIF11
und CULFW32), ist SPI-Ethernet der Pfad.

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

Auf dem ESP32-C6 ist der einzige sinnvolle SPI-Bus FSPI (User-SPI).
Wenn das HM-Modul kein SPI braucht (HM-MOD-RPI-PCB ist UART-only,
Type-7-Frames laufen über UART) → kein Konflikt, FSPI exklusiv für
W5500. PIIF11-Pinout reserviert SPI0 für HAT-SPI; in einem
**Stick-Formfaktor ohne Pi-Header** ist diese Reservierung obsolet,
SPI darf an der gleichen Pin-Gruppe an den W5500 gehen.

## Power-Budget

- ESP32-C6-MINI-1: TX-Peak ~280 mA bei WiFi.
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
- Form: USB-A-Stecker direkt am Board (klassischer Stick-Look) oder
  USB-C-Buchse + RJ45-Buchse als kleines Box-Gehäuse?
- mDNS-Service-Type — `_raw-uart._udp` (HB-RF-ETH) bleibt; soll zusätzlich
  `_rfnethm._tcp` o.ä. mit Capabilities-Record exportiert werden?
- HmIP-RFUSB-formatkompatibles Gehäuse anstreben (Ergonomie) oder eigenes
  Gehäuse?
