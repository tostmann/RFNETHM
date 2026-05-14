# RFNETHM — Diagramm-Quellen

Mermaid-Quellen für Architektur- und Message-Flow-Diagramme.
Text-Source ins Git, rendert nativ auf GitHub und in den meisten
Markdown-Viewern; Export zu PNG/SVG für Slides via `mmdc`.

## Inhaltsverzeichnis

| Datei | Was es zeigt | Doc-Anker |
|---|---|---|
| `architecture.mmd` | Top-Level: Stick → Bridge → 4 Sinks → Linux-Clients | `decisions.md` „Drei Bauformen" |
| `hm_frame_rx.mmd` | RX-Pfad-Sequence: Funk → Stick → Bridge-Fanout an alle Sinks | `firmware_architecture.md` (memory) |
| `hm_frame_tx.mmd` | TX-Pfad mit TX-Master-Soft-Lock; AUTO/PINNED-Entscheidung | `tx_master_lock.md` (memory) |
| `hbrfeth_protocol.mmd` | UDP/3008 wire-protocol: V2-CONNECT, KeepAlive, FRAME, Resume | `hb_rf_eth_kernel_quirks.md` (memory) |
| `hmuartlgw_legacy.mmd` | TCP/2330 FHEM/Homegear-Bridge, Phasen A–D | `hmuartlgw_legacy_emu.md` (memory) |
| `tx_master_states.mmd` | TX-Master-Lock State-Machine (AUTO/PINNED) | `tx_master_lock.md` (memory) |
| `improv_idle_window.mmd` | Improv-Serial-Window mit Idle-Bookkeeping | `improv_idle_window.md` (memory) |

## Pre-rendered SVGs

`rendered/*.svg` enthält die mit `mmdc` 11.14 erzeugten Renders aller
sieben Diagramme (theme=neutral, 2026-05-07).  Direkt in
GitHub-Web-Vorschau anschaubar, in Slides per `<img src=…>` einbettbar.
Bei Source-Änderung neu generieren (siehe `mmdc`-Block unten).

## Rendern

### GitHub-Web

Die `.mmd`-Sources rendern *nicht* von alleine in der GitHub-Vorschau —
sie sind reine Mermaid-Quellen.  Wenn man sie inline in eine README
einbettet (in einem ` ```mermaid `-Block), rendert GitHub sie nativ.

### mermaid.live (Browser, no-install)

1. Datei-Inhalt kopieren.
2. <https://mermaid.live> öffnen, einfügen.
3. PNG / SVG exportieren.

### `mmdc` (lokal, für Batch-Export zu PNG/SVG)

```sh
npm install -g @mermaid-js/mermaid-cli
```

**ARM64 (Pi 5 etc.):** der von npm mitgebrachte chrome-headless-shell
ist x86_64-only.  System-Chromium nutzen via puppeteer-config:

```sh
cat > /tmp/puppeteer-config.json <<'EOF'
{ "executablePath": "/usr/bin/chromium",
  "args": ["--no-sandbox", "--disable-setuid-sandbox"] }
EOF
```

Batch-Render aller .mmd zu SVG:

```sh
cd docs/diagrams
mkdir -p rendered
for f in *.mmd; do
  mmdc -i "$f" -o "rendered/${f%.mmd}.svg" -t neutral \
       -p /tmp/puppeteer-config.json
done
```

PNG für Slides (4K-Auflösung, weißer Hintergrund):

```sh
for f in *.mmd; do
  mmdc -i "$f" -o "rendered/${f%.mmd}.png" -t neutral \
       -p /tmp/puppeteer-config.json -w 2400 -s 2 -b white
done
```

Die Kombination `-w 2400 -s 2` ist wichtig: `-w` allein kappt die
natürliche SVG-Größe, `-s 2` allein nimmt nur den Default-Viewport
(~800 px) als Basis.  Erst beide zusammen → 4K-druckbare PNGs
(~3000–4000 px Längskante je nach Diagramm).

`rendered/*.png` sind unter Git versioniert für direkte Slide-
Einbettung; bei Source-Änderung mit obigem Block neu generieren.

### Inline in Slides (Reveal.js / Marp)

```markdown
\`\`\`mermaid
%%{init: {'theme':'neutral'}}%%
flowchart LR
  A --> B
\`\`\`
```

## Konventionen

- **Theme:** `neutral` als Default (gut druckbar, hoher Kontrast für
  Beamer).  Andere Themes (`forest`, `dark`) per `%%{init}%%`-Header
  pro Datei überschreibbar.
- **Subgraph-Farben:** über `classDef` statt inline-Styles, damit
  Themen-Wechsel sauber bleibt.
- **Sequenz-Diagramme:** `rect rgb(...)`-Blöcke fassen Phasen zusammen
  (siehe `hbrfeth_protocol.mmd`, `hmuartlgw_legacy.mmd`) — bessere
  Lesbarkeit als plain notes.
- **Quelltext-Anker** in Header-Kommentaren der `.mmd`-Files: zeigt
  welche `.c/.h`-Datei die Logik implementiert.
