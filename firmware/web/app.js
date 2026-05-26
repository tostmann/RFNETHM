// RFNETHM dashboard — periodic /api/status fetch + tile updates.

const $ = (id) => document.getElementById(id);

function fmtUptime(sec) {
  sec = sec | 0;
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function fmtBytes(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(2) + " MB";
}

function setResetClass(reason) {
  const el = $("brand-reset");
  el.classList.remove("brand-reset-ok", "brand-reset-warn", "brand-reset-crash");
  if (reason === "PANIC" || reason === "INT_WDT" || reason === "TASK_WDT" || reason === "WDT") {
    el.classList.add("brand-reset-crash");
  } else if (reason === "BROWNOUT") {
    el.classList.add("brand-reset-warn");
  } else {
    el.classList.add("brand-reset-ok");
  }
}

function setStateBadge(connected) {
  const el = $("badge-state");
  el.classList.remove("status-online", "status-offline", "status-unknown");
  if (connected === true)  { el.classList.add("status-online");  el.textContent = "online"; }
  else if (connected === false) { el.classList.add("status-offline"); el.textContent = "offline"; }
  else { el.classList.add("status-unknown"); el.textContent = "…"; }
}

function setUsbPill(s) {
  const el = $("usb-state-pill");
  el.classList.remove("ready", "boot", "idle", "err");
  if (!s.connected) { el.classList.add("idle"); el.textContent = "kein Stick"; return; }
  if (s.crc_err > 0) { el.classList.add("err"); el.textContent = "CRC-Fehler"; return; }
  if (s.boot)        { el.classList.add("ready"); el.textContent = "ready"; return; }
  el.classList.add("boot"); el.textContent = "boot probe";
}

function setUartPill(s) {
  const el = $("uart-state-pill");
  el.classList.remove("ready", "boot", "idle", "err");
  if (!s.present) { el.classList.add("idle"); el.textContent = "kein Modul"; return; }
  if (s.crc_err > 0) { el.classList.add("err"); el.textContent = "CRC-Fehler"; return; }
  if (s.boot)        { el.classList.add("ready"); el.textContent = "ready"; return; }
  el.classList.add("boot"); el.textContent = "boot probe";
}

// Markiert die aktuell aktive Source mit "active"-CSS-Klasse + Pill.
function setActiveSource(active) {
  const usbCard  = $("card-usb");
  const uartCard = $("card-uart");
  const usbPill  = $("usb-active-pill");
  const uartPill = $("uart-active-pill");
  usbCard.classList.toggle("rf-active",  active === "usb");
  uartCard.classList.toggle("rf-active", active === "uart");
  usbPill.textContent  = (active === "usb")  ? "ACTIVE" : "";
  usbPill.classList.toggle("on", active === "usb");
  uartPill.textContent = (active === "uart") ? "ACTIVE" : "";
  uartPill.classList.toggle("on", active === "uart");
}

function setNetMode(ap) {
  const el = $("st-net-mode");
  el.classList.remove("sta", "ap");
  if (ap) { el.classList.add("ap");  el.textContent = "AP"; }
  else    { el.classList.add("sta"); el.textContent = "STA"; }
}

async function tick() {
  try {
    const r = await fetch("/api/status", { cache: "no-store" });
    if (!r.ok) throw new Error("status " + r.status);
    const j = await r.json();

    // Headbar
    $("brand-version").textContent = "v" + j.fw.version;
    $("brand-uptime").textContent  = fmtUptime(j.sys.uptime_s);
    $("brand-reset").textContent   = j.sys.reset_reason;
    setResetClass(j.sys.reset_reason);
    setStateBadge(!!j.net.up);

    // Heap-Trend + Stack-HWM + Coredump-Marker (Dauerlauf-Observability)
    if (j.sys.min_free_heap !== undefined) {
      const heapEl = $("brand-heap");
      heapEl.textContent = "heap " + fmtBytes(j.sys.free_heap) + " / min " + fmtBytes(j.sys.min_free_heap);
      heapEl.classList.toggle("brand-heap-low", j.sys.min_free_heap < 16 * 1024);
    }
    if (j.sys.stack_min_words !== undefined) {
      const stEl = $("brand-stack");
      stEl.textContent = "stk " + (j.sys.stack_min_name || "?") + ":" + j.sys.stack_min_words + "w";
      stEl.classList.toggle("brand-stack-low", j.sys.stack_min_words < 256);
    }
    if (j.sys.coredump !== undefined) {
      const cdEl = $("brand-coredump");
      cdEl.classList.toggle("brand-coredump-hidden", !j.sys.coredump);
    }

    // RF-Sources (USB + UART)
    setUsbPill(j.usb);
    $("usb-conn").textContent  = j.usb.connected ? "verbunden" : "—";
    $("usb-boot").textContent  = j.usb.boot ? "ja" : "nein";
    $("usb-tag").textContent   = j.usb.tag || "—";
    $("usb-frames").textContent = j.usb.frames_ok;
    $("usb-crc").textContent    = j.usb.crc_err;
    $("usb-trunc").textContent  = j.usb.trunc;
    $("usb-skip").textContent   = j.usb.skip;

    if (j.uart) {
      setUartPill(j.uart);
      $("uart-conn").textContent  = j.uart.present ? "präsent" : "—";
      $("uart-boot").textContent  = j.uart.boot ? "ja" : "nein";
      $("uart-tag").textContent   = j.uart.tag || "—";
      $("uart-frames").textContent = j.uart.frames_ok;
      $("uart-crc").textContent    = j.uart.crc_err;
      $("uart-trunc").textContent  = j.uart.trunc;
      $("uart-skip").textContent   = j.uart.skip;
    }
    setActiveSource(j.src && j.src.active);

    // Net tile
    setNetMode(!!j.net.ap);
    $("st-ip").textContent       = j.net.ip || "0.0.0.0";
    $("st-ssid").textContent     = "SSID " + (j.net.ssid || "(none)");
    $("st-host").textContent     = j.net.host || "—";
    if (j.net.ap) {
      $("st-net-state").textContent = "Captive AP";
    } else {
      $("st-net-state").textContent = j.net.up ? "connected" : "disconnected";
    }
    // mDNS-Name (Hostname + .local) als klickbarer Link.
    // Im AP-Mode bringt mDNS nichts → Link ausgrauen.
    const mdnsEl = $("st-mdns-link");
    if (j.net.host) {
      const fqdn = j.net.host + ".local";
      mdnsEl.textContent = fqdn;
      if (j.net.ap) {
        mdnsEl.removeAttribute("href");
        mdnsEl.classList.add("mdns-disabled");
        mdnsEl.title = "mDNS gilt nicht im AP-Mode";
      } else {
        mdnsEl.href = "http://" + fqdn + "/";
        mdnsEl.classList.remove("mdns-disabled");
        mdnsEl.title = "Open via mDNS — funktioniert auf Mac/Linux/iOS direkt, Windows braucht Bonjour-Service";
      }
    } else {
      mdnsEl.textContent = "—";
      mdnsEl.removeAttribute("href");
    }

    // HB-RF-ETH tile
    $("st-hb-port").textContent    = j.hb.port;
    $("st-hb-clients").textContent = j.hb.clients;
    $("st-hb-conn").textContent    = j.hb.connects;
    $("st-hb-disc").textContent    = j.hb.disconnects;
    $("st-hb-rx").textContent      = j.hb.rx;
    $("st-hb-tx").textContent      = j.hb.tx;
    $("st-hb-ka").textContent      = j.hb.ka_timeouts;
    $("st-hb-crc").textContent     = j.hb.bad_crc;

    // Raw-TCP tile
    $("st-tcp-port").textContent    = j.tcp.port;
    $("st-tcp-clients").textContent = j.tcp.clients;
    $("st-tcp-acc").textContent     = j.tcp.accepts;
    $("st-tcp-rx").textContent      = fmtBytes(j.tcp.rx);
    $("st-tcp-tx").textContent      = fmtBytes(j.tcp.tx);

    // Bridge tile
    $("st-br-sinks").textContent = j.br.sinks;
    $("st-br-rx").textContent    = fmtBytes(j.br.rx_bytes) + " / " + j.br.rx_pumps + " pumps";
    $("st-br-tx").textContent    = fmtBytes(j.br.tx_bytes);
    $("st-br-drop").textContent  = j.br.tx_drop;

    // TX-Master state — sync radios + Bridge tile rows
    if (j.tx) {
      const masterEl = $("st-br-master");
      const sinceLabel = (j.tx.since_ms < 0)
          ? "—"
          : (j.tx.since_ms < 1000 ? j.tx.since_ms + " ms"
             : (j.tx.since_ms / 1000).toFixed(1) + " s");
      const ownerLabel = j.tx.owner || "—";
      masterEl.textContent = (j.tx.mode === "pinned" ? "PIN" : "auto") +
                             " · " + ownerLabel + " (last " + sinceLabel + ")";
      $("st-br-rejlock").textContent = j.tx.rej_total;
      // Reflect mode in the radios — only if user isn't currently dragging
      // a click (= we just sent a POST and are waiting for echo).
      if (!window.__tx_master_pending) {
        const desired = (j.tx.mode === "pinned") ? j.tx.owner : "auto";
        const radios = document.querySelectorAll('input[name=tx-master]');
        radios.forEach(r => { r.checked = (r.value === desired); });
        // Card-Glow setzen + Out-of-Lock-Karten ausgrauen (nur im PIN-Mode).
        document.querySelectorAll('.stats-card').forEach(c => {
          c.classList.remove("tx-pinned");
          c.classList.remove("tx-locked-out");
        });
        const sel = document.querySelector('input[name=tx-master][value="' + desired + '"]');
        if (sel) {
          const card = sel.closest(".stats-card");
          if (card) card.classList.add("tx-pinned");
        }
        if (j.tx.mode === "pinned") {
          // Alle TX-Kacheln (mit eigenem Radio, value != "auto") grauen
          // außer der gepinnte Owner.
          document.querySelectorAll('input[name=tx-master]').forEach(r => {
            if (r.value === "auto") return;     // Bridge-Card nie ausgrauen
            if (r.value === desired) return;
            const card = r.closest(".stats-card");
            if (card) card.classList.add("tx-locked-out");
          });
        }
      }
    }

    // HMUARTLGW Legacy tile (Phase A-D Stats)
    if (j.hmu) {
      $("st-hmu-port").textContent    = j.hmu.port;
      $("st-hmu-clients").textContent = j.hmu.clients;
      $("st-hmu-conn").textContent    = j.hmu.connects + " / " + j.hmu.disconnects;
      $("st-hmu-tx").textContent      = j.hmu.tx + (j.hmu.reject ? "  (reject " + j.hmu.reject + ")" : "");
      $("st-hmu-ack").textContent     = j.hmu.ack;
      $("st-hmu-orph").textContent    = j.hmu.orph;
      $("st-hmu-foreign").textContent = (j.hmu.foreign === undefined ? "—" : j.hmu.foreign);
      $("st-hmu-rx").textContent      = j.hmu.rx;
      $("st-hmu-aes").textContent     = j.hmu.aes;
      // Health-Pill: nur echte orphaned ACKs (wir waren tx-master, aber
      // pending-Bucket war leer) sind das Smoking-Gun-Signal.  Foreign-
      // ACKs (= TX kam von einem anderen Sink, z.B. HB-RF-ETH) sind im
      // Multi-Sink-Betrieb normal und dürfen die Pille nicht rot färben.
      const pill = $("st-hmu-warn");
      pill.classList.remove("ready", "boot", "idle", "err");
      if (j.hmu.orph > 0) {
        pill.classList.add("err"); pill.textContent = "orph " + j.hmu.orph;
      } else if (j.hmu.clients > 0) {
        pill.classList.add("ready"); pill.textContent = "active";
      } else if (j.hmu.connects > 0) {
        pill.classList.add("idle"); pill.textContent = "idle";
      } else {
        pill.textContent = "";
      }
    }

    // System tile
    $("st-uptime").textContent = fmtUptime(j.sys.uptime_s);
    $("st-heap").textContent   = fmtBytes(j.sys.free_heap);
    $("st-reset").textContent  = j.sys.reset_reason;
    $("st-built").textContent  = j.fw.built;

    // Footer
    $("footer-build").textContent = `v${j.fw.version} · built ${j.fw.built}`;
  } catch (e) {
    setStateBadge(null);
    $("brand-uptime").textContent = "API offline";
  }
}

// ============ WiFi-Modal ============
function openWifiModal() {
  $("wifi-modal").classList.add("open");
  $("wifi-modal-msg").textContent = "";
  $("wifi-modal-msg").className = "modal-msg";
}
function closeWifiModal() {
  $("wifi-modal").classList.remove("open");
}

$("btn-wifi-cfg").addEventListener("click", openWifiModal);
$("wifi-modal-close").addEventListener("click", closeWifiModal);
$("wifi-modal").addEventListener("click", (ev) => {
  if (ev.target.id === "wifi-modal") closeWifiModal();
});

async function scanWifi() {
  const btn = $("btn-wifi-scan");
  const sel = $("wifi-scan-select");
  const msg = $("wifi-modal-msg");
  btn.disabled = true;
  const oldLabel = btn.textContent;
  btn.textContent = "scanne…";
  msg.className = "modal-msg";
  msg.textContent = "Scan läuft (ca. 4–6 s)…";
  try {
    const r = await fetch("/api/wifi/scan", { cache: "no-store" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    const list = await r.json();
    // De-dup auf SSID, stärkstes Signal gewinnt
    const uniq = {};
    list.forEach(n => {
      if (!n.ssid) return;
      if (!uniq[n.ssid] || uniq[n.ssid].rssi < n.rssi) uniq[n.ssid] = n;
    });
    const sorted = Object.values(uniq).sort((a,b) => b.rssi - a.rssi);
    sel.innerHTML = "";
    if (sorted.length === 0) {
      sel.innerHTML = "<option value=''>(nichts gefunden)</option>";
    } else {
      const opt0 = document.createElement("option");
      opt0.value = ""; opt0.textContent = "— wählen —";
      sel.appendChild(opt0);
      sorted.forEach(n => {
        const o = document.createElement("option");
        o.value = n.ssid;
        const lock = n.auth && n.auth !== "open" ? "🔒 " : "";
        o.textContent = `${lock}${n.ssid}  (${n.rssi} dBm, ch${n.ch})`;
        sel.appendChild(o);
      });
    }
    msg.className = "modal-msg ok";
    msg.textContent = `${sorted.length} SSIDs gefunden`;
  } catch (e) {
    msg.className = "modal-msg error";
    msg.textContent = "Scan fehlgeschlagen: " + e.message;
  } finally {
    btn.textContent = oldLabel;
    btn.disabled = false;
  }
}

$("btn-wifi-scan").addEventListener("click", scanWifi);
$("wifi-scan-select").addEventListener("change", (ev) => {
  if (ev.target.value) {
    $("wifi-modal-ssid").value = ev.target.value;
    $("wifi-modal-pass").focus();
  }
});

async function connectWifi() {
  const ssid = $("wifi-modal-ssid").value.trim();
  const pass = $("wifi-modal-pass").value;
  const msg  = $("wifi-modal-msg");
  msg.className = "modal-msg";
  if (!ssid) {
    msg.classList.add("error");
    msg.textContent = "SSID darf nicht leer sein";
    return;
  }
  msg.textContent = "speichere…";
  try {
    const r = await fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ssid, pass })
    });
    if (!r.ok) throw new Error("HTTP " + r.status);
    msg.className = "modal-msg ok";
    msg.textContent = "gespeichert — Reboot in 1 s, danach Connect zu " + ssid;
    setTimeout(() => fetch("/api/reboot", { method: "POST" }).catch(() => {}), 1000);
  } catch (e) {
    msg.className = "modal-msg error";
    msg.textContent = "Fehler: " + e.message;
  }
}
$("btn-wifi-connect").addEventListener("click", connectWifi);

async function resetWifi() {
  if (!confirm("Gespeicherte WLAN-Daten löschen und im Captive-AP-Mode neustarten?")) return;
  const msg = $("wifi-modal-msg");
  msg.className = "modal-msg";
  msg.textContent = "lösche Creds…";
  try {
    const r = await fetch("/api/wifi/reset", { method: "POST" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    msg.className = "modal-msg ok";
    msg.textContent = "Creds gelöscht — Reboot in den AP-Mode…";
  } catch (e) {
    msg.className = "modal-msg error";
    msg.textContent = "Fehler: " + e.message;
  }
}
$("btn-wifi-reset").addEventListener("click", resetWifi);

// ============ Reboot ============
$("btn-reboot").addEventListener("click", async () => {
  if (!confirm("RFNETHM neu starten?")) return;
  try {
    await fetch("/api/reboot", { method: "POST" });
    $("badge-state").textContent = "rebooting…";
  } catch (e) { /* expected — connection drops */ }
});

// ============ OTA ============
$("ota-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const file = $("ota-file").files[0];
  const msg = $("ota-msg");
  msg.className = "config-msg";
  if (!file) { msg.classList.add("error"); msg.textContent = "keine Datei"; return; }
  msg.textContent = "uploade " + fmtBytes(file.size) + "…";
  try {
    const r = await fetch("/api/ota", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: file
    });
    if (r.ok) {
      const j = await r.json().catch(() => ({}));
      msg.classList.add("ok");
      msg.textContent = "OK — " + (j.bytes || file.size) + " B geschrieben, Reboot…";
    } else {
      msg.classList.add("error");
      msg.textContent = "OTA fehlgeschlagen (" + r.status + ")";
    }
  } catch (e) {
    msg.classList.add("error");
    msg.textContent = "OTA-Fehler: " + e.message;
  }
});

// ============ Tabs ============
const tabBtns = document.querySelectorAll(".tab");
const tabPanes = { stats: $("tab-stats"), console: $("tab-console") };
function activateTab(name) {
  tabBtns.forEach(b => b.classList.toggle("active", b.dataset.tab === name));
  Object.entries(tabPanes).forEach(([k, el]) => el.classList.toggle("active", k === name));
  if (name === "console") logTickSoon();
}
tabBtns.forEach(b => b.addEventListener("click", () => activateTab(b.dataset.tab)));

// ============ Debug-Console (Polling /api/log?since=N) ============
let logSeq = 0;
let logTimer = null;
let logPaused = false;
const logPane    = $("console-pane");
const logState   = $("console-state");
const logInfo    = $("console-info");
const logScroll  = $("log-autoscroll");

function setLogState(cls, txt) {
  logState.classList.remove("ready", "live", "paused", "dead", "boot", "idle", "err");
  logState.classList.add(cls);
  logState.textContent = txt;
}

function escHTML(s) {
  return s.replace(/[&<>]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));
}

function levelClass(msg) {
  if (msg.startsWith("E "))      return "ll-e";
  if (msg.startsWith("W "))      return "ll-w";
  if (msg.startsWith("D "))      return "ll-d";
  if (msg.startsWith("I "))      return "ll-i";
  // ESP_LOG-style "I (12345) tag: …"
  const m = msg.match(/^([IWED]) \((\d+)\) /);
  if (m) return "ll-" + m[1].toLowerCase();
  return "";
}

function appendLine(seq, ts, msg) {
  const span = document.createElement("span");
  const cls  = levelClass(msg);
  if (cls) span.className = cls;
  span.innerHTML = `<span class="ll-ts">${ts.toString().padStart(8," ")}</span>${escHTML(msg)}\n`;
  logPane.appendChild(span);
  // Trim to last ~600 lines visually
  while (logPane.childNodes.length > 600) {
    logPane.removeChild(logPane.firstChild);
  }
}

async function logTick() {
  if (logPaused) { logTimer = setTimeout(logTick, 1500); return; }
  try {
    const r = await fetch("/api/log?since=" + logSeq, { cache: "no-store" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    const j = await r.json();

    if (logSeq === 0 && j.head > 0) {
      // First poll: clear placeholder, ask for the buffer-window-tail.
      logPane.textContent = "";
      logSeq = Math.max(0, j.oldest);
      // We'll get the actual lines on the next iteration; loop fast.
      setTimeout(logTick, 50);
      return;
    }

    if (j.lines && j.lines.length) {
      for (const e of j.lines) appendLine(e.seq, e.ts, e.msg);
      logSeq = j.head;
      if (logScroll.checked) logPane.scrollTop = logPane.scrollHeight;
    } else {
      logSeq = j.head;
    }
    setLogState("live", "live · seq " + j.head);
    logInfo.className = "config-msg";
    logInfo.textContent = `head=${j.head}  oldest=${j.oldest}`;
  } catch (e) {
    setLogState("dead", "API offline");
    logInfo.className = "config-msg error";
    logInfo.textContent = "Fehler: " + e.message;
  }
  logTimer = setTimeout(logTick, 1000);
}
function logTickSoon() {
  if (logTimer) return;
  logTimer = setTimeout(logTick, 50);
}

$("btn-log-clear").addEventListener("click", () => {
  logPane.textContent = "";
});

// ============ TX-Master radios ============
async function postTxMaster(value) {
  window.__tx_master_pending = true;
  try {
    const r = await fetch("/api/bridge/master", {
      method:  "POST",
      headers: { "Content-Type": "application/json" },
      body:    JSON.stringify({ sink: value })
    });
    if (!r.ok) {
      console.warn("tx-master POST failed:", r.status);
    }
  } catch (e) {
    console.warn("tx-master POST error:", e);
  } finally {
    // Kurzes Cooldown — verhindert, dass die nächste tick() das Radio
    // überschreibt, BEVOR der Server den neuen Status reflektiert.
    setTimeout(() => { window.__tx_master_pending = false; }, 1500);
    // Sofort einen tick triggern, damit User direktes Feedback sieht
    tick();
  }
}
document.querySelectorAll('input[name=tx-master]').forEach(r => {
  r.addEventListener("change", (ev) => {
    if (ev.target.checked) postTxMaster(ev.target.value);
  });
});

// Initial + interval
tick();
setInterval(tick, 2000);
// Console-Tab-Polling startet nur wenn der Tab aktiviert ist (s.o.).

// ───── Online-Update-Check ─────────────────────────────────────────────
// Beim Page-Load 1× refresh=1 anstoßen (Device fetcht install.busware.de/
// rfnethm/manifest.json), danach alle 6 h cached neu prüfen.  Wenn
// update_available true ist, brand-update-Badge in der Navbar zeigen
// und mit der gefundenen Version beschriften.
async function checkForUpdate(forceRefresh) {
  try {
    const url = forceRefresh ? "/api/update/check?refresh=1" : "/api/update/check";
    const r = await fetch(url, { cache: "no-store" });
    if (!r.ok) return;
    const j = await r.json();
    const el = $("brand-update");
    if (!el) return;
    if (j.update_available && j.available) {
      el.textContent = "⬆ v" + j.available;
      el.title = `Neuere Firmware v${j.available} verfügbar (aktuell v${j.current}). Klick öffnet den Webflasher.`;
      el.classList.remove("brand-update-hidden");
    } else {
      el.classList.add("brand-update-hidden");
    }
  } catch (e) { /* network failure: badge bleibt hidden */ }
}
// Initial-Check kurz nach Boot warten (WiFi muss up sein), dann
// periodisch alle 6 h.
setTimeout(() => checkForUpdate(true), 2500);
setInterval(() => checkForUpdate(true), 6 * 60 * 60 * 1000);

// ───── Theme-Toggle ─────────────────────────────────────────────────────
// Inline-Script im <head> setzt data-theme schon vor dem ersten Paint
// (FOUC-frei) und nutzt localStorage 'rfnethm-theme'.  Hier nur noch:
//   1) Toggle-Button verdrahten — schaltet light↔dark + persist
//   2) OS-Wechsel beobachten — wird nur dann angewendet wenn der User
//      keine explizite Wahl getroffen hat (= localStorage leer)
const THEME_KEY = "rfnethm-theme";
function applyTheme(t) {
  document.documentElement.setAttribute("data-theme", t === "dark" ? "dark" : "light");
}
const themeBtn = $("theme-toggle");
if (themeBtn) {
  themeBtn.addEventListener("click", () => {
    const cur = document.documentElement.getAttribute("data-theme") === "dark" ? "dark" : "light";
    const next = cur === "dark" ? "light" : "dark";
    applyTheme(next);
    try { localStorage.setItem(THEME_KEY, next); } catch (e) {}
  });
}
if (window.matchMedia) {
  const mq = window.matchMedia("(prefers-color-scheme: dark)");
  // moderner Listener — Safari < 14 hatte addListener, hier ignorieren.
  if (mq.addEventListener) {
    mq.addEventListener("change", (ev) => {
      let stored = null;
      try { stored = localStorage.getItem(THEME_KEY); } catch (e) {}
      if (stored !== "light" && stored !== "dark") {
        applyTheme(ev.matches ? "dark" : "light");
      }
    });
  }
}
