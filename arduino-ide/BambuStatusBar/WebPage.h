#pragma once

// Bambu Status Bar web UI: hero, status badges, collapsible sections,
// amber primary actions. APIs stay local to this Status Bar firmware.
const char INDEX_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bambu Status Bar</title>
<style>
:root{--bg:#0b1015;--panel:#121a23;--panel-2:#172230;--line:#263548;--text:#eef4fb;--muted:#99a9bb;--accent:#f0a64b;--accent-2:#42c291;--warn:#f5c24f;--danger:#ff6b6b}
*{box-sizing:border-box}html,body{margin:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:radial-gradient(circle at top,#182433 0,#0b1015 55%);color:var(--text);padding:24px}
main{max-width:1040px;margin:0 auto;display:grid;gap:18px}
h1,h2,h3,p{margin:0}label{display:block;margin:0 0 8px;font-size:14px;font-weight:600;color:#d5e1ed}
input,select{width:100%;padding:13px 14px;border-radius:14px;border:1px solid var(--line);background:#0f1721;color:var(--text);outline:none;font:inherit}
input:focus,select:focus{border-color:#4d86c7;box-shadow:0 0 0 3px rgba(77,134,199,.18)}
input[type=color]{min-height:54px;padding:6px 10px;background:#0c131b}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;background:#1e3347;border-radius:3px;border:none;padding:0;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:18px;height:18px;border-radius:50%;background:#94a3b8}
button{border:none;border-radius:999px;padding:13px 18px;font-weight:700;cursor:pointer;font:inherit}
button:disabled{opacity:.65;cursor:wait}
button.primary{background:linear-gradient(135deg,#f0a64b,#ffd07a);color:#1b1203}
button.secondary{background:#203044;color:var(--text);border:1px solid #34506e}
.hero,.section,.footer-card{background:rgba(18,26,35,.94);border:1px solid var(--line);border-radius:24px;box-shadow:0 14px 32px rgba(0,0,0,.2)}
.hero{padding:24px;display:grid;gap:18px}
.section{padding:0;overflow:hidden}
.section-body{padding:0 22px 22px;display:grid;gap:16px}
.footer-card{padding:18px;display:grid;gap:8px}
.stack{display:grid;gap:18px}
.hero-top{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
.hero-brand{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.eyebrow{font-size:18px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;color:#b9d4ef}
.hero-version-row{display:inline-flex;align-items:center;gap:8px;flex-wrap:wrap}
.hero-version,.hero-update{display:inline-flex;align-items:center;justify-content:center;padding:6px 12px;border-radius:999px;font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;text-decoration:none}
.hero-version{border:1px solid #31557d;background:#111b29;color:#dceafa}
.hero-update{border:1px solid #8a5a1a;background:#2a1c0a;color:#ffd07a;cursor:pointer}
.hero-update[hidden]{display:none!important}
.title{font-size:34px;line-height:1.05}.subtitle{max-width:720px;color:var(--muted);line-height:1.55}
.micro,.section-summary p{color:var(--muted);line-height:1.5}.micro{font-size:13px}
.section-summary{display:grid;grid-template-columns:1fr auto;gap:6px 14px;align-items:center;padding:22px;cursor:pointer;list-style:none}
.section-summary::-webkit-details-marker{display:none}
.section-summary h2{grid-column:1;grid-row:1;margin:0;font-size:22px}
.section-summary p{grid-column:1/-1;grid-row:2;margin:0}
.section-summary-side{grid-column:2;grid-row:1;display:flex;align-items:center;gap:10px}
.summary-pill{display:inline-flex;align-items:center;justify-content:center;padding:8px 12px;border-radius:999px;border:1px solid var(--line);font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;background:#0f1721;color:var(--text);white-space:nowrap}
.summary-pill.ok{border-color:#22614c;background:#10231d}
.summary-pill.warn{border-color:#7d6222;background:#241c0e}
.summary-pill.info{border-color:#31557d;background:#111b29}
.summary-pill.idle{border-color:#334456;background:#121a23}
.section-toggle-icon{width:32px;height:32px;border-radius:999px;border:1px solid #365064;background:#0f1721;display:grid;place-items:center;flex:0 0 auto}
.section-toggle-icon::before{content:'+';font-size:20px;line-height:1;color:#cfe0f1}
details[open]>.section-summary .section-toggle-icon::before{content:'-'}
.hint-box{padding:14px 16px;border-radius:18px;background:#0e1620;border:1px solid #2b3e54;color:var(--muted)}
.hint-box strong{color:var(--text)}
.badge-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
.badge{padding:14px 16px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:6px}
.badge-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91a6bc}
.badge-value{font-size:15px;font-weight:700;line-height:1.35}
.badge.ok{border-color:#22614c;background:#10231d}
.badge.warn{border-color:#7d6222;background:#241c0e}
.badge.info{border-color:#31557d;background:#111b29}
.badge.idle{border-color:#334456;background:#121a23}
.grid-2{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,460px),1fr));gap:14px}
.grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}
.field{display:grid;gap:8px}
.actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center}
.actions .micro{min-width:180px}
.color-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px}
.light-rows{display:grid;gap:12px;margin-top:8px}
.light-row{display:grid;grid-template-columns:140px minmax(0,1fr);gap:12px;align-items:end}
@media(max-width:640px){.light-row{grid-template-columns:1fr}}
.check{display:flex;align-items:center;justify-content:space-between;gap:14px;margin:0;padding:12px 14px;border-radius:16px;border:1px solid var(--line);background:#0f1721;cursor:pointer}
.check>span{flex:1;min-width:0;font-size:14px;font-weight:600;color:#d5e1ed;line-height:1.35}
.check input[type=checkbox]{appearance:none;-webkit-appearance:none;width:46px;height:28px;flex:0 0 auto;margin:0;padding:0;border-radius:999px;border:1px solid #34506e;background:#1a2836;position:relative;cursor:pointer;transition:background .2s ease,border-color .2s ease,box-shadow .2s ease}
.check input[type=checkbox]::after{content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#94a3b8;box-shadow:0 1px 3px rgba(0,0,0,.35);transition:transform .2s ease,background .2s ease}
.check input[type=checkbox]:checked{background:linear-gradient(135deg,#f0a64b,#ffd07a);border-color:#c48a3a}
.check input[type=checkbox]:checked::after{transform:translateX(18px);background:#1b1203}
.check input[type=checkbox]:focus-visible{outline:none;box-shadow:0 0 0 3px rgba(240,166,75,.28)}
.status-line{font-size:16px;font-weight:700}
.status-detail{color:var(--muted)}
.printer-card{padding:16px 18px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:8px;margin-bottom:10px;cursor:pointer;text-align:left;width:100%;color:inherit;font:inherit}
.printer-card:hover{border-color:#4d86c7}
.ota-bar{display:none;margin:4px 0;height:8px;border-radius:6px;background:#0e1620;overflow:hidden}
.ota-bar>i{display:block;height:100%;width:0;background:var(--accent);transition:width .25s}
@media(max-width:840px){.badge-grid,.color-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:640px){body{padding:16px}.title{font-size:28px}.badge-grid,.grid-2,.grid-3,.color-grid{grid-template-columns:1fr}.hero,.footer-card{padding:18px}.section-summary{padding:18px}.section-summary-side{width:100%;justify-content:space-between}.section-body{padding:0 18px 18px}}
</style>
</head>
<body>
<main>
<section class="hero">
  <div class="hero-top">
    <div class="hero-brand">
      <div class="eyebrow" data-i="hero_eyebrow">Web config</div>
      <div class="hero-version-row">
        <span class="hero-version" id="heroVersion">…</span>
        <a class="hero-update" id="heroUpdate" href="#secOta" hidden></a>
      </div>
    </div>
  </div>
  <h1 class="title" data-i="title">Bambu Status Bar</h1>
  <p class="subtitle" id="subtitle" data-i="subtitle">WLAN, lokaler Drucker und LED-Leiste für deinen Bambu Lab Drucker.</p>
  <div class="badge-grid">
    <div class="badge idle" id="wifiBadge"><div class="badge-label" data-i="badge_wifi">Wi-Fi</div><div class="badge-value" id="wifiBadgeVal">…</div></div>
    <div class="badge idle" id="localBadge"><div class="badge-label" data-i="badge_local">Local Path</div><div class="badge-value" id="localBadgeVal">…</div></div>
    <div class="badge idle" id="stateBadge"><div class="badge-label" data-i="badge_state">Status</div><div class="badge-value" id="stateBadgeVal">…</div></div>
    <div class="badge idle" id="apBadge"><div class="badge-label" data-i="badge_ap">Hotspot</div><div class="badge-value" id="apBadgeVal">…</div></div>
  </div>
  <div class="hint-box" id="heroHint"><strong data-i="note">Note:</strong> <span id="heroHintText"></span></div>
</section>

<form class="stack" id="configForm" onsubmit="return false">
<details class="section" id="secWifi" open>
  <summary class="section-summary">
    <h2 data-i="sec_wifi">Wi-Fi</h2>
    <p data-i="sec_wifi_desc">Dieses Netzwerk trägt die LED-Leiste und den lokalen Druckerzugriff.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="wifiPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="actions">
      <button type="button" class="secondary" id="wifiScan" data-i="scan">Scan Networks</button>
      <div class="micro" id="wifiScanDetail" data-i="scan_hint">Scan visible networks or type an SSID manually.</div>
    </div>
    <div class="field" id="wifiSelectField" style="display:none"><label for="wifiSelect" data-i="detected">Detected Networks</label><select id="wifiSelect"><option value="">…</option></select></div>
    <div class="grid-2">
      <div class="field"><label for="ssid" data-i="ssid">Wi-Fi SSID</label><input id="ssid" maxlength="32" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label for="wifiPass" data-i="password">Wi-Fi Password</label><input id="wifiPass" type="password" maxlength="64" autocomplete="off" placeholder=""></div>
    </div>
    <div class="grid-2">
      <div class="field"><label for="host" data-i="hostname">Hostname</label><input id="host" maxlength="32" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label>&nbsp;</label><button type="button" class="secondary" id="hostBtn" data-i="set_hostname">Set Hostname</button></div>
    </div>
    <p class="micro" data-i="hostname_hint">Fritzbox-safe: underscores become hyphens.</p>
    <p class="micro"><span data-i="wifi_status_label">Current network status:</span> <span id="wifiDetail">…</span></p>
    <div class="actions">
      <button type="button" class="primary" id="wifiBtn" data-i="connect">Connect Wi-Fi</button>
      <div class="micro" data-i="wifi_connect_hint">Saves credentials and starts the join immediately. The setup hotspot stays available if the join fails.</div>
    </div>
  </div>
</details>

<details class="section" id="secPrinter">
  <summary class="section-summary">
    <h2 data-i="sec_printer">Local Printer Path</h2>
    <p data-i="sec_printer_desc">Lokales MQTT liefert Live-Status, Fortschritt und Heizphasen für die LED-Leiste.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="localPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="actions">
      <button type="button" class="secondary" id="printerScan" data-i="printer_scan">Scan Printers</button>
      <div class="micro" id="printerScanDetail" data-i="printer_scan_hint">Listens ~12 s for Bambu NOTIFY broadcasts (UDP 2021). Needs Wi-Fi first.</div>
    </div>
    <div id="printerList"></div>
    <div class="grid-2">
      <div class="field"><label for="printerIp" data-i="printer_ip">Printer IP or Hostname</label><input id="printerIp" maxlength="40" autocapitalize="off" autocomplete="off"></div>
      <div class="field"><label for="printerSerial" data-i="serial">Printer Serial Number</label><input id="printerSerial" maxlength="32" autocapitalize="characters" autocomplete="off"></div>
    </div>
    <div class="field"><label for="printerCode" data-i="code">Access Code</label><input id="printerCode" type="password" maxlength="16" autocomplete="off"></div>
    <div class="hint-box"><strong data-i="local_status">Local Status:</strong> <span id="mqttDetail">…</span></div>
    <div class="actions">
      <button type="button" class="primary" id="bindBtn" data-i="bind">Connect Local</button>
      <div class="micro" data-i="bind_hint">Saves printer credentials and reconnects MQTT without reboot.</div>
    </div>
  </div>
</details>

<details class="section" id="secLight">
  <summary class="section-summary">
    <h2 data-i="sec_light">Light Colors</h2>
    <p data-i="sec_light_desc">Helligkeit, Geschwindigkeit und Statusfarben der LED-Leiste. Änderungen gelten sofort.</p>
    <div class="section-summary-side"><span class="summary-pill info" id="lightPill" data-i="pill_live">Live</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <p class="micro" data-i="light_hint">Farbe und Animation pro Status. Helligkeit und Geschwindigkeit gelten für alle. Fertig bleibt 15 Minuten, danach Leerlauf.</p>
    <div class="grid-2">
      <div class="field"><label for="brightness"><span data-i="light_brightness">Brightness</span> <span id="briVal"></span></label><input id="brightness" type="range" min="0" max="100" value="50"></div>
      <div class="field"><label for="speed"><span data-i="light_speed">Speed</span> <span id="spdVal"></span></label><input id="speed" type="range" min="1" max="100" value="50"></div>
    </div>
    <div class="grid-2">
      <div class="field"><label for="ledPin" data-i="light_pin">LED data pin (GPIO)</label><input id="ledPin" type="number" min="0" max="48" step="1" value="19"></div>
      <div class="field"><label for="ledCount" data-i="light_count">LED count</label><input id="ledCount" type="number" min="1" max="64" step="1" value="18"></div>
    </div>
    <p class="micro" data-i="light_hw_hint">Pin or count changes reboot the device so FastLED can re-init.</p>
    <label class="check"><span data-i="light_reverse">Reverse LED direction</span><input id="ledReverse" type="checkbox"></label>
    <label class="check"><span data-i="light_follow_chamber">Follow printer chamber light</span><input id="followChamberLight" type="checkbox"></label>
    <p class="micro" id="chamberLightHint">When enabled, the status bar turns off while the printer chamber light is off.</p>
    <div class="light-rows" id="lightStates"></div>
    <div class="actions"><button type="button" class="secondary" id="lightReset" data-i="light_reset">Reset Colors</button></div>
  </div>
</details>

<details class="section" id="secAp">
  <summary class="section-summary">
    <h2 data-i="sec_ap">Setup Hotspot</h2>
    <p data-i="sec_ap_desc">Fallback-Access-Point, falls kein WLAN erreichbar ist.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="apPill">…</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <label class="check"><span data-i="ap_enable">Keep hotspot enabled when Wi-Fi is up</span><input id="apEnabled" type="checkbox"></label>
    <div class="grid-2">
      <div class="field"><label for="apSsid" data-i="ap_ssid">Hotspot Name</label><input id="apSsid" maxlength="32" autocapitalize="off"></div>
      <div class="field"><label for="apPass" data-i="ap_pass">Hotspot Password</label><input id="apPass" maxlength="64" autocomplete="off"></div>
    </div>
    <div class="field"><label for="apIp" data-i="ap_ip">Hotspot IP</label><input id="apIp" maxlength="15" inputmode="decimal"></div>
    <div class="hint-box" data-i="ap_hint">Without Wi-Fi the hotspot stays on even if disabled here. Empty password = open, otherwise at least 8 characters.</div>
    <div class="actions">
      <button type="button" class="primary" id="apBtn" data-i="ap_save">Save + Restart</button>
      <div class="micro" data-i="ap_save_hint">Applies hotspot settings on the next boot.</div>
    </div>
  </div>
</details>

<details class="section" id="secOta">
  <summary class="section-summary">
    <h2 data-i="sec_ota">Firmware Update</h2>
    <p data-i="sec_ota_desc">Update von GitHub Releases oder per lokaler .bin-Datei.</p>
    <div class="section-summary-side"><span class="summary-pill idle" id="otaPill" data-i="pill_ota">OTA</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="hint-box"><strong data-i="warn">Warning:</strong> <span data-i="ota_hint">Das Gerät startet nach dem Flash neu. Nutze nur firmware.bin (ESP32-C6), nicht firmware.factory.bin.</span></div>
    <div class="field"><label data-i="ota_github">GitHub Update</label>
      <div class="micro" id="otaVersionInfo">—</div>
    </div>
    <div class="actions">
      <button type="button" class="secondary" id="otaCheckBtn" data-i="ota_check">Auf Update prüfen</button>
      <button type="button" class="primary" id="otaInstallBtn" data-i="ota_install" disabled>Update installieren</button>
      <button type="button" class="secondary" id="otaReinstallBtn" data-i="ota_reinstall" disabled>Neuinstallation</button>
    </div>
    <div class="micro" id="otaGhStatus" data-i="ota_gh_ready">Prüft die neueste Release auf GitHub.</div>
    <hr style="border:none;border-top:1px solid #243447;margin:18px 0">
    <div class="field"><label for="fw" data-i="ota_file">Firmware .bin Datei</label><input id="fw" type="file" accept=".bin,application/octet-stream"></div>
    <div class="ota-bar" id="otaProg"><i id="otaBar"></i></div>
    <div class="actions">
      <button type="button" class="secondary" id="otaBtn" data-i="ota_start">Hochladen &amp; flashen</button>
      <div class="micro" id="otaStatus" data-i="ota_ready">Oder lokal eine .bin für ESP32-C6 wählen.</div>
    </div>
  </div>
</details>

<details class="section" id="secDevice">
  <summary class="section-summary">
    <h2 data-i="sec_device">Device</h2>
    <p data-i="sec_device_desc">Sprache und Werkseinstellungen.</p>
    <div class="section-summary-side"><span class="summary-pill info" data-i="pill_device">Device</span><span class="section-toggle-icon"></span></div>
  </summary>
  <div class="section-body">
    <div class="field"><label for="language" data-i="lang">Language</label>
      <select id="language"><option value="de">Deutsch</option><option value="en">English</option></select>
    </div>
    <div class="actions">
      <button type="button" class="secondary" id="resetBtn" data-i="factory">Factory Reset</button>
      <div class="micro" data-i="boot_hint">Or hold BOOT for 3 seconds while the device is already running.</div>
    </div>
  </div>
</details>

<section class="footer-card">
  <div class="status-line" id="status">…</div>
  <div class="status-detail" id="statusDetail"></div>
</section>
</form>
</main>
<script>
const I18N={
de:{
title:"Bambu Status Bar",hero_eyebrow:"Web config",subtitle:"WLAN, lokaler Drucker und LED-Leiste für deinen Bambu Lab Drucker.",
note:"Hinweis:",hint_ap:"Im Setup-Hotspot reicht zuerst Wi-Fi. Danach öffnest du diese Seite im Heimnetz.",
hint_ok:"Wi-Fi ist online. Druckerbindung, Licht und OTA kannst du unten einstellen.",
badge_wifi:"Wi-Fi",badge_local:"Local Path",badge_state:"Status",badge_ap:"Hotspot",
sec_wifi:"Wi-Fi",sec_wifi_desc:"Dieses Netzwerk trägt die LED-Leiste und den lokalen Druckerzugriff.",
scan:"Netzwerke suchen",scan_hint:"Sichtbar scannen oder SSID manuell eingeben.",detected:"Gefundene Netzwerke",
ssid:"Wi-Fi SSID",password:"Wi-Fi Passwort",hostname:"Hostname",set_hostname:"Hostname setzen",
hostname_hint:"Unterstriche → Bindestriche. Die Fritzbox behält den alten Namen oft sticky: Heimnetz → Netzwerk → Gerät entfernen, dann neu verbinden.",
wifi_status_label:"Aktueller Netzwerkstatus:",connect:"Wi-Fi verbinden",
wifi_connect_hint:"Speichert die Zugangsdaten und startet die Verbindung sofort. Der Setup-Hotspot bleibt bei Fehler verfügbar.",
sec_printer:"Local Printer Path",sec_printer_desc:"Lokales MQTT liefert Live-Status, Fortschritt und Heizphasen für die LED-Leiste.",
printer_scan:"Drucker suchen",printer_scan_hint:"Lauscht ~12 s auf Bambu-NOTIFY-Broadcasts (UDP 2021). Zuerst Wi-Fi verbinden.",
printer_ip:"Drucker-IP oder Hostname",serial:"Seriennummer",code:"Access Code",
local_status:"Local Status:",bind:"Lokal verbinden",bind_hint:"Speichert die Druckerdaten und verbindet MQTT ohne Neustart.",
sec_light:"Light Colors",sec_light_desc:"Helligkeit, Geschwindigkeit, Animationen und Statusfarben. Änderungen gelten sofort.",
pill_live:"Live",light_brightness:"Helligkeit",light_speed:"Geschwindigkeit",
light_idle:"Leerlauf",light_print:"Druck",light_error:"Fehler",
light_homing:"Referenzfahrt",light_leveling:"Nivellieren",light_preparing:"Vorbereitung",
light_heating:"Heizen",light_downloading:"Download",light_paused:"Pause",light_finished:"Fertig",
light_idle_fx:"Animation",light_print_fx:"Animation",light_error_fx:"Animation",
light_homing_fx:"Animation",light_leveling_fx:"Animation",light_preparing_fx:"Animation",
light_heating_fx:"Animation",light_downloading_fx:"Animation",light_paused_fx:"Animation",light_finished_fx:"Animation",
light_reverse:"LED-Richtung umkehren",
light_follow_chamber:"Mit Drucker-Chamber-Licht synchronisieren",
light_follow_hint:"Wenn aktiv, bleibt die Statusleiste aus, solange das Chamber-Licht am Drucker aus ist (Fehler leuchtet weiter).",
light_chamber_state:"Chamber-Licht",
light_pin:"LED-Datenpin (GPIO)",light_count:"LED-Anzahl",
light_hw_hint:"Änderung von Pin oder Anzahl startet das Gerät neu (FastLED-Init).",
fx_pulse:"Atmen",fx_solid:"Fest",fx_comet:"Komet",fx_wave:"Welle",fx_strobe:"Blinken",fx_progress:"Fortschritt",
fx_rainbow:"Regenbogen",fx_sparkle:"Funkeln",fx_fire:"Feuer",
light_reset:"Farben & Animationen zurücksetzen",light_hint:"Farbe und Animation pro Status. Helligkeit und Geschwindigkeit gelten für alle. Fertig bleibt 15 Minuten, danach Leerlauf.",
sec_ap:"Setup Hotspot",sec_ap_desc:"Fallback-Access-Point, falls kein WLAN erreichbar ist.",
ap_enable:"Hotspot auch bei verbundenem Wi-Fi anlassen",ap_ssid:"Hotspot-Name",ap_pass:"Hotspot-Passwort",ap_ip:"Hotspot-IP",
ap_hint:"Ohne Wi-Fi bleibt der Hotspot an, auch wenn er hier aus ist. Passwort leer = offen, sonst mindestens 8 Zeichen.",
ap_save:"Speichern + Neustart",ap_save_hint:"Hotspot-Einstellungen gelten nach dem Neustart.",
sec_ota:"Firmware Update",sec_ota_desc:"Update von GitHub Releases oder per lokaler .bin-Datei.",
pill_ota:"OTA",warn:"Warnung:",ota_file:"Firmware-.bin-Datei",ota_start:"Hochladen & flashen",
ota_hint:"Das Gerät startet nach dem Flash neu. Nutze nur firmware.bin (ESP32-C6), nicht firmware.factory.bin.",
ota_ready:"Oder lokal eine .bin für ESP32-C6 wählen.",
ota_github:"GitHub Update",ota_check:"Auf Update prüfen",ota_install:"Update installieren",
ota_reinstall:"Neuinstallation",
ota_gh_ready:"Prüft die neueste Release auf GitHub.",
ota_up_to_date:"Du bist auf dem neuesten Stand.",ota_available:"Update verfügbar.",
ota_checking:"Prüfe GitHub…",ota_installing:"Lade Firmware und flashe…",
ota_reinstalling:"Installiere aktuelle Release erneut…",
hero_update:"Update",
err_no_wifi:"Zuerst mit Wi-Fi verbinden",err_no_release:"Kein Release gefunden",err_no_asset:"Kein firmware.bin im Release",
err_http_begin:"Verbindung fehlgeschlagen",err_http_connect:"GitHub nicht erreichbar (TLS/DNS)",err_update_begin:"OTA konnte nicht starten",err_update_end:"OTA fehlgeschlagen",
err_short_write:"Download unvollständig",err_bad_version:"Versionsformat ungültig",err_bad_json:"Ungültige Antwort",
err_bad_request:"Ungültige Anfrage",err_bad_lang:"Sprache ungültig",err_udp_bind:"UDP-Bind fehlgeschlagen",
err_ota_busy:"OTA läuft bereits",err_bad_url:"URL ungültig",err_not_found:"Nicht gefunden",
err_bad_led_pin:"GPIO-Pin nicht unterstützt",err_bad_led_count:"LED-Anzahl ungültig",
sec_device:"Device",sec_device_desc:"Sprache und Werkseinstellungen.",pill_device:"Device",lang:"Sprache",
factory:"Werkseinstellungen",boot_hint:"Oder BOOT am laufenden Gerät 3 Sekunden halten.",
wifi_on:"Verbunden",wifi_off:"Nicht verbunden",wifi_try:"Verbinde…",wifi_fail:"Wi-Fi fehlgeschlagen",
mqtt_on:"MQTT verbunden",mqtt_off:"Nicht verbunden",mqtt_wait:"Warte auf Druckerdaten",
ap_on:"Aktiv",ap_off:"Aus",saved:"Gespeichert",rebooting:"Neustart… Seite danach neu laden.",
ota_ok:"Update fertig, Neustart…",ota_fail:"Update fehlgeschlagen",
factory_ask:"Alle Einstellungen löschen und neu starten?",
state_provisioning:"Nicht eingerichtet",state_wifi:"Suche Wi-Fi",state_mqtt:"Verbinde Drucker",
state_idle:"Leerlauf",state_homing:"Referenzfahrt",state_leveling:"Nivellieren",state_preparing:"Vorbereitung",
state_downloading:"Download",state_heating:"Heizen",state_printing:"Druck",state_paused:"Pause",
state_finished:"Fertig",state_error:"Fehler",state_standby:"Standby",state_unknown:"—",
err_bad_ssid:"SSID ungültig",err_bad_pass:"Passwort ungültig",
err_ap_pass:"Hotspot-Passwort: leer oder ≥ 8 Zeichen",err_bad_ip:"IP ungültig",
err_bad_host:"Hostname: Buchstaben, Zahlen, Bindestrich oder Unterstrich",err_bad_printer:"Druckerdaten ungültig",
err_bad_leds:"Lichtwerte ungültig"
},
en:{
title:"Bambu Status Bar",hero_eyebrow:"Web config",subtitle:"Wi-Fi, local printer path and LED bar for your Bambu Lab printer.",
note:"Note:",hint_ap:"While the setup hotspot is up, start with Wi-Fi. Then reopen this page on the home-network IP.",
hint_ok:"Wi-Fi is up. Review printer binding, light colors and OTA below.",
badge_wifi:"Wi-Fi",badge_local:"Local Path",badge_state:"Status",badge_ap:"Hotspot",
sec_wifi:"Wi-Fi",sec_wifi_desc:"This network carries the LED bar and the local printer path.",
scan:"Scan Networks",scan_hint:"Scan for visible networks or type an SSID manually.",detected:"Detected Networks",
ssid:"Wi-Fi SSID",password:"Wi-Fi Password",hostname:"Hostname",set_hostname:"Set Hostname",
hostname_hint:"Underscores become hyphens. Fritzbox often keeps the first name: Heimnetz → Netzwerk → remove device, then reconnect.",
wifi_status_label:"Current network status:",connect:"Connect Wi-Fi",
wifi_connect_hint:"Saves credentials and starts the join immediately. The setup hotspot stays available if the join fails.",
sec_printer:"Local Printer Path",sec_printer_desc:"Local MQTT provides live status, progress and heating phases for the LED bar.",
printer_scan:"Scan Printers",printer_scan_hint:"Listens ~12 s for Bambu NOTIFY broadcasts (UDP 2021). Needs Wi-Fi first.",
printer_ip:"Printer IP or Hostname",serial:"Printer Serial Number",code:"Access Code",
local_status:"Local Status:",bind:"Connect Local",bind_hint:"Saves printer credentials and reconnects MQTT without reboot.",
sec_light:"Light Colors",sec_light_desc:"Brightness, speed, animations and status colors. Changes apply live.",
pill_live:"Live",light_brightness:"Brightness",light_speed:"Speed",
light_idle:"Idle",light_print:"Printing",light_error:"Error",
light_homing:"Homing",light_leveling:"Leveling",light_preparing:"Preparing",
light_heating:"Heating",light_downloading:"Downloading",light_paused:"Paused",light_finished:"Finished",
light_idle_fx:"Animation",light_print_fx:"Animation",light_error_fx:"Animation",
light_homing_fx:"Animation",light_leveling_fx:"Animation",light_preparing_fx:"Animation",
light_heating_fx:"Animation",light_downloading_fx:"Animation",light_paused_fx:"Animation",light_finished_fx:"Animation",
light_reverse:"Reverse LED direction",
light_follow_chamber:"Follow printer chamber light",
light_follow_hint:"When enabled, the status bar turns off while the printer chamber light is off (errors still light up).",
light_chamber_state:"Chamber light",
light_pin:"LED data pin (GPIO)",light_count:"LED count",
light_hw_hint:"Changing pin or count reboots the device so FastLED can re-init.",
fx_pulse:"Pulse",fx_solid:"Solid",fx_comet:"Comet",fx_wave:"Wave",fx_strobe:"Strobe",fx_progress:"Progress",
fx_rainbow:"Rainbow",fx_sparkle:"Sparkle",fx_fire:"Fire",
light_reset:"Reset colors & animations",light_hint:"Color and animation per status. Brightness and speed apply to all. Finished stays for 15 minutes, then idle.",
sec_ap:"Setup Hotspot",sec_ap_desc:"Fallback access point when home Wi-Fi is unavailable.",
ap_enable:"Keep hotspot enabled when Wi-Fi is up",ap_ssid:"Hotspot Name",ap_pass:"Hotspot Password",ap_ip:"Hotspot IP",
ap_hint:"Without Wi-Fi the hotspot stays on even if disabled here. Empty password = open, otherwise at least 8 characters.",
ap_save:"Save + Restart",ap_save_hint:"Hotspot settings apply on the next boot.",
sec_ota:"Firmware Update",sec_ota_desc:"Update from GitHub Releases or a local .bin file.",
pill_ota:"OTA",warn:"Warning:",ota_file:"Firmware .bin file",ota_start:"Upload & Flash",
ota_hint:"The device restarts after flashing. Use firmware.bin (ESP32-C6) only — not firmware.factory.bin.",
ota_ready:"Or pick a local .bin built for ESP32-C6.",
ota_github:"GitHub update",ota_check:"Check for update",ota_install:"Install update",
ota_reinstall:"Reinstall",
ota_gh_ready:"Checks the latest release on GitHub.",
ota_up_to_date:"You are up to date.",ota_available:"Update available.",
ota_checking:"Checking GitHub…",ota_installing:"Downloading and flashing…",
ota_reinstalling:"Reinstalling the latest release…",
hero_update:"Update",
err_no_wifi:"Connect to Wi-Fi first",err_no_release:"No release found",err_no_asset:"No firmware.bin in release",
err_http_begin:"Connection failed",err_http_connect:"GitHub unreachable (TLS/DNS)",err_update_begin:"OTA could not start",err_update_end:"OTA failed",
err_short_write:"Incomplete download",err_bad_version:"Invalid version format",err_bad_json:"Invalid response",
err_bad_request:"Invalid request",err_bad_lang:"Invalid language",err_udp_bind:"UDP bind failed",
err_ota_busy:"OTA already in progress",err_bad_url:"Invalid URL",err_not_found:"Not found",
err_bad_led_pin:"GPIO pin not supported",err_bad_led_count:"Invalid LED count",
sec_device:"Device",sec_device_desc:"Language and factory reset.",pill_device:"Device",lang:"Language",
factory:"Factory Reset",boot_hint:"Or hold BOOT for 3 seconds while the device is already running.",
wifi_on:"Connected",wifi_off:"Not connected",wifi_try:"Connecting…",wifi_fail:"Wi-Fi failed",
mqtt_on:"MQTT connected",mqtt_off:"Not connected",mqtt_wait:"Waiting for printer details",
ap_on:"Active",ap_off:"Off",saved:"Saved",rebooting:"Rebooting… reload the page afterwards.",
ota_ok:"Update complete, rebooting…",ota_fail:"Update failed",
factory_ask:"Erase all settings and reboot?",
state_provisioning:"Not set up",state_wifi:"Joining Wi-Fi",state_mqtt:"Connecting printer",
state_idle:"Idle",state_homing:"Homing",state_leveling:"Leveling",state_preparing:"Preparing",
state_downloading:"Downloading",state_heating:"Heating",state_printing:"Printing",state_paused:"Paused",
state_finished:"Finished",state_error:"Error",state_standby:"Standby",state_unknown:"—",
err_bad_ssid:"Invalid SSID",err_bad_pass:"Invalid password",
err_ap_pass:"Hotspot password: empty or ≥ 8 characters",err_bad_ip:"Invalid IP",
err_bad_host:"Hostname: letters, digits, hyphen or underscore",err_bad_printer:"Invalid printer details",
err_bad_leds:"Invalid light values"
}};
let lang="de", pollTimer=null, ledTimer=null, openedOnce=false;
const $=id=>document.getElementById(id);
const LIGHT_SLOTS=[
  {key:"idle",color:"#ffffff",fx:"pulse"},
  {key:"homing",color:"#ff7800",fx:"comet"},
  {key:"leveling",color:"#ff7800",fx:"comet"},
  {key:"preparing",color:"#ff7800",fx:"comet"},
  {key:"heating",color:"#ff5000",fx:"progress"},
  {key:"downloading",color:"#ffdc00",fx:"comet"},
  {key:"print",color:"#ffffff",fx:"progress"},
  {key:"paused",color:"#ffffff",fx:"pulse"},
  {key:"finished",color:"#00ff00",fx:"solid"},
  {key:"error",color:"#ff0000",fx:"strobe"}
];
const FX_LIST=["pulse","solid","comet","wave","strobe","progress","rainbow","sparkle","fire"];
function t(k){return (I18N[lang]&&I18N[lang][k])||I18N.de[k]||k}
function fxOptionsHtml(selected){
  return FX_LIST.map(v=>'<option value="'+v+'" data-i="fx_'+v+'"'+(v===selected?' selected':'')+'>'+t('fx_'+v)+'</option>').join('');
}
function buildLightRows(){
  const root=$("lightStates"); if(!root||root.dataset.ready) return;
  root.innerHTML=LIGHT_SLOTS.map(s=>{
    const colorId=s.key+'Color', fxId=s.key+'Effect';
    return '<div class="light-row">'+
      '<div class="field"><label for="'+colorId+'" data-i="light_'+s.key+'">'+t('light_'+s.key)+'</label>'+
      '<input id="'+colorId+'" type="color" value="'+s.color+'"></div>'+
      '<div class="field"><label for="'+fxId+'" data-i="light_'+s.key+'_fx">'+t('light_'+s.key+'_fx')+'</label>'+
      '<select id="'+fxId+'">'+fxOptionsHtml(s.fx)+'</select></div></div>';
  }).join('');
  root.dataset.ready='1';
  LIGHT_SLOTS.forEach(s=>{
    $(s.key+'Color').addEventListener('input',scheduleLeds);
    $(s.key+'Effect').addEventListener('change',scheduleLeds);
  });
}
function applyLang(){
  document.documentElement.lang=lang;
  document.querySelectorAll("[data-i]").forEach(el=>{el.textContent=t(el.dataset.i)});
  LIGHT_SLOTS.forEach(s=>{
    const sel=$(s.key+'Effect'); if(!sel) return;
    [...sel.options].forEach(o=>{ if(o.dataset.i) o.textContent=t(o.dataset.i); });
  });
}
function setStatus(line,detail){$("status").textContent=line||"";$("statusDetail").textContent=detail||""}
function errText(code){return t("err_"+String(code||"").replace(/-/g,"_"))}
async function api(path,body){
  const opt={method:"POST",headers:{"Content-Type":"application/json"}};
  if(body!==undefined) opt.body=JSON.stringify(body);
  const res=await fetch(path,opt);
  const data=await res.json().catch(()=>({}));
  if(!res.ok||data.ok===false) throw new Error(data.error||"bad-json");
  return data;
}
function setIdle(id,value,opts){
  const el=$(id); if(!el||document.activeElement===el) return;
  if(el.type==="checkbox"){ el.checked=!!value; return; }
  // Status polling must not wipe values the user just filled from a scan card
  // with empty NVS (printer not bound yet).
  if(opts&&opts.keepIfEmpty&&!value&&el.value) return;
  // Keep a scan/manual choice that differs from the saved/connected value.
  if(opts&&opts.keepIfLocal&&el.value&&el.value!==String(value||"")) return;
  el.value=value;
}
function setBadge(el,cls){el.className="badge "+cls}
function setPill(el,text,cls){el.textContent=text;el.className="summary-pill "+cls}
function stateLabel(data){
  let key="state_"+(data.state||"unknown");
  if(data.asleep&&(data.state==="idle"||data.state==="finished")) key="state_standby";
  let text=t(key);
  if((data.state==="printing"||data.state==="heating")&&typeof data.progress==="number") text+=" "+data.progress+"%";
  return text;
}
function applyStatus(data){
  if(data.language&&data.language!==lang){lang=data.language;applyLang()}
  const wifi=data.wifi||{}, ap=data.ap||{}, printer=data.printer||{}, leds=data.leds||{};
  $("heroVersion").textContent=data.version||"";
  const upd=data.update||{};
  const heroUp=$("heroUpdate");
  if(upd.url) otaDownloadUrl=upd.url;
  if(upd.available&&upd.latest){
    heroUp.hidden=false;
    heroUp.textContent=t("hero_update")+" "+upd.latest;
    setOtaInfo(data.version||"",upd.latest,true);
  }else{
    heroUp.hidden=true;
    heroUp.textContent="";
    if(upd.checked&&upd.latest) setOtaInfo(data.version||"",upd.latest,false);
    else $("otaReinstallBtn").disabled=!otaDownloadUrl;
  }
  const wifiOk=!!wifi.connected, mqttOk=!!printer.mqtt, printerReady=!!printer.ready;
  setBadge($("wifiBadge"), wifi.connecting?"warn":(wifiOk?"ok":(wifi.failed?"warn":"idle")));
  $("wifiBadgeVal").textContent=wifi.connecting?(t("wifi_try")+(wifi.target||"")):(wifiOk?(wifi.ssid||t("wifi_on")):t("wifi_off"));
  setBadge($("localBadge"), mqttOk?"ok":(printerReady?"warn":"idle"));
  $("localBadgeVal").textContent=mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait"));
  const st=stateLabel(data);
  setBadge($("stateBadge"), data.state==="error"?"warn":(wifiOk?"info":"idle"));
  $("stateBadgeVal").textContent=st;
  setBadge($("apBadge"), ap.running?"info":"idle");
  $("apBadgeVal").textContent=ap.running?((ap.ssid||t("ap_on"))):t("ap_off");
  $("heroHintText").textContent=wifiOk?t("hint_ok"):t("hint_ap");
  setPill($("wifiPill"), wifiOk?(wifi.ip||t("wifi_on")):t("wifi_off"), wifiOk?"ok":(wifi.failed?"warn":"idle"));
  setPill($("localPill"), mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait")), mqttOk?"ok":(printerReady?"warn":"idle"));
  setPill($("apPill"), ap.running?t("ap_on"):t("ap_off"), ap.running?"info":"idle");
  $("wifiDetail").textContent=wifi.connecting?(t("wifi_try")+(wifi.target||"")):(wifiOk?(wifi.ssid+" · "+wifi.ip):t("wifi_off"));
  $("mqttDetail").textContent=mqttOk?t("mqtt_on"):(printerReady?t("mqtt_off"):t("mqtt_wait"));
  setIdle("host", data.hostname||"");
  setIdle("ssid", wifi.ssid||"", {keepIfEmpty:true, keepIfLocal:true});
  setIdle("printerIp", printer.ip||"", {keepIfEmpty:true});
  setIdle("printerSerial", printer.serial||"", {keepIfEmpty:true});
  if(document.activeElement!==$("printerCode") && printer.code) $("printerCode").placeholder="••••••••";
  buildLightRows();
  if(document.activeElement!==$("brightness")) $("brightness").value=leds.brightness??50;
  if(document.activeElement!==$("speed")) $("speed").value=leds.speed??50;
  LIGHT_SLOTS.forEach(s=>{
    const def=s;
    setIdle(s.key+'Color', leds[s.key]||def.color);
    setIdle(s.key+'Effect', leds[s.key+'Effect']||def.fx);
  });
  setIdle("ledReverse", !!leds.reverse);
  setIdle("followChamberLight", !!leds.followChamberLight);
  const ch=printer.chamberLight||"unknown";
  const chLabel=ch==="on"?(lang==="de"?"an":"on"):(ch==="off"?(lang==="de"?"aus":"off"):"—");
  $("chamberLightHint").textContent=t("light_follow_hint")+" ("+t("light_chamber_state")+": "+chLabel+")";
  if(document.activeElement!==$("ledPin")) $("ledPin").value=leds.pin??19;
  if(document.activeElement!==$("ledCount")){
    $("ledCount").max=leds.maxCount||64;
    $("ledCount").value=leds.count??18;
  }
  $("briVal").textContent=$("brightness").value+"%";
  $("spdVal").textContent=$("speed").value+"%";
  setIdle("apEnabled", !!ap.enabled);
  setIdle("apSsid", ap.ssid||"");
  setIdle("apPass", ap.password||"");
  setIdle("apIp", ap.ip||"");
  setIdle("language", lang);
  let line=st;
  if(wifiOk) line+=" · "+wifi.ip; else if(ap.running) line+=" · "+ap.ip;
  setStatus(line, wifi.failed?t("wifi_fail"):"");
  if(!openedOnce){
    openedOnce=true;
    $("secWifi").open=!wifiOk;
    $("secPrinter").open=wifiOk&&!mqttOk;
    $("secLight").open=wifiOk&&mqttOk;
  }
}
async function refresh(){try{const res=await fetch("/api/status",{cache:"no-store"});applyStatus(await res.json())}catch(e){}}
function startPoll(){if(!pollTimer) pollTimer=setInterval(refresh,2000)}
async function pollList(startPath,listPath,render){
  await api(startPath,{});
  for(let i=0;i<40;i++){
    const res=await fetch(listPath,{cache:"no-store"});
    const data=await res.json();
    render(data);
    if(!data.scanning) return;
    await new Promise(r=>setTimeout(r,400));
  }
}
function renderWifi(data){
  const sel=$("wifiSelect");
  sel.innerHTML='<option value="">'+(lang==="de"?"Netzwerk wählen…":"Select detected Wi-Fi…")+'</option>';
  (data.networks||[]).forEach(n=>{
    const o=document.createElement("option");
    o.value=n.ssid; o.textContent=n.ssid+"  "+n.rssi+" dBm"+(n.secure?" *":"");
    sel.appendChild(o);
  });
  $("wifiSelectField").style.display=(data.networks&&data.networks.length)?"grid":"none";
  $("wifiScanDetail").textContent=data.scanning?(lang==="de"?"Suche…":"Scanning…"):t("scan_hint");
}
function renderPrinters(data){
  const box=$("printerList"); box.innerHTML="";
  (data.printers||[]).forEach(p=>{
    const b=document.createElement("button");
    b.type="button"; b.className="printer-card";
    b.innerHTML="<strong>"+(p.name||p.serial||"Bambu")+"</strong><span class='micro'>"+p.ip+(p.serial?" · "+p.serial:"")+"</span>";
    b.onclick=()=>{ $("printerIp").value=p.ip||""; if(p.serial) $("printerSerial").value=p.serial; };
    box.appendChild(b);
  });
  $("printerScanDetail").textContent=data.scanning?(lang==="de"?"Suche…":"Scanning…"):t("printer_scan_hint");
}
function scheduleLeds(){clearTimeout(ledTimer);ledTimer=setTimeout(saveLeds,250)}
async function saveLeds(){
  buildLightRows();
  const body={
    brightness:Number($("brightness").value),
    speed:Number($("speed").value),
    reverse:$("ledReverse").checked,
    followChamberLight:$("followChamberLight").checked,
    pin:Number($("ledPin").value),
    count:Number($("ledCount").value)
  };
  LIGHT_SLOTS.forEach(s=>{ body[s.key]=$(s.key+'Color').value; body[s.key+'Effect']=$(s.key+'Effect').value; });
  try{
    const d=await api("/api/leds",body);
    setStatus(d.reboot?t("rebooting"):t("saved"),"");
    if(d.reboot)clearInterval(pollTimer);
  }catch(e){setStatus(errText(e.message),"")}
}
$("wifiSelect").onchange=()=>{if($("wifiSelect").value) $("ssid").value=$("wifiSelect").value};
$("wifiScan").onclick=async()=>{
  $("wifiScan").disabled=true;
  try{await pollList("/api/wifi/scan","/api/wifi/scan",renderWifi)}catch(e){setStatus(errText(e.message),"")}
  $("wifiScan").disabled=false;
};
$("wifiBtn").onclick=async()=>{
  try{await api("/api/wifi",{ssid:$("ssid").value,password:$("wifiPass").value});setStatus(t("wifi_try")+$("ssid").value,"")}
  catch(e){setStatus(errText(e.message),"")}
};
$("hostBtn").onclick=async()=>{
  try{const d=await api("/api/hostname",{hostname:$("host").value.trim()});setStatus(d.reboot?t("rebooting"):t("saved"),"");if(d.reboot)clearInterval(pollTimer)}
  catch(e){setStatus(errText(e.message),"")}
};
$("printerScan").onclick=async()=>{
  $("printerScan").disabled=true;
  try{await pollList("/api/printer/scan","/api/printer/scan",renderPrinters)}catch(e){setStatus(errText(e.message),"")}
  $("printerScan").disabled=false;
};
$("bindBtn").onclick=async()=>{
  try{await api("/api/printer",{ip:$("printerIp").value.trim(),serial:$("printerSerial").value.trim(),code:$("printerCode").value.trim()});setStatus(t("saved"),"");$("printerCode").value=""}
  catch(e){setStatus(errText(e.message),"")}
};
["brightness","speed"].forEach(id=>{
  $(id).addEventListener("input",()=>{$("briVal").textContent=$("brightness").value+"%";$("spdVal").textContent=$("speed").value+"%";scheduleLeds()});
});
["ledPin","ledCount"].forEach(id=>$(id).addEventListener("change",scheduleLeds));
$("ledReverse").addEventListener("change",scheduleLeds);
$("followChamberLight").addEventListener("change",scheduleLeds);
$("lightReset").onclick=async()=>{try{await api("/api/leds/reset",{});setStatus(t("saved"),"");refresh()}catch(e){setStatus(errText(e.message),"")}};
buildLightRows();
$("apBtn").onclick=async()=>{
  try{const d=await api("/api/ap",{enabled:$("apEnabled").checked,ssid:$("apSsid").value.trim(),password:$("apPass").value,ip:$("apIp").value.trim()});setStatus(d.reboot?t("rebooting"):t("saved"),"");if(d.reboot)clearInterval(pollTimer)}
  catch(e){setStatus(errText(e.message),"")}
};
$("language").onchange=async()=>{lang=$("language").value;applyLang();try{await api("/api/language",{language:lang})}catch(e){setStatus(errText(e.message),"")}};
let otaDownloadUrl="";
function setOtaInfo(current,latest,available){
  const cur=current||"—", lat=latest||"—";
  $("otaVersionInfo").textContent=(lang==="de"?"Aktuell: ":"Current: ")+cur+" · "+(lang==="de"?"Neueste: ":"Latest: ")+lat;
  $("otaInstallBtn").disabled=!available;
  $("otaReinstallBtn").disabled=!otaDownloadUrl;
  setPill($("otaPill"), available?(lang==="de"?"Update":"Update"):t("pill_ota"), available?"warn":"idle");
}
async function runGithubInstall(reinstall){
  if(!otaDownloadUrl){ $("otaGhStatus").textContent=t("ota_gh_ready"); return; }
  $("otaInstallBtn").disabled=true;
  $("otaReinstallBtn").disabled=true;
  $("otaCheckBtn").disabled=true;
  $("otaGhStatus").textContent=reinstall?t("ota_reinstalling"):t("ota_installing");
  $("otaProg").style.display="block"; $("otaBar").style.width="35%";
  try{
    await api("/api/update/github",{url:otaDownloadUrl});
    $("otaBar").style.width="100%";
    setStatus(t("ota_ok"),""); $("otaGhStatus").textContent=t("ota_ok");
    clearInterval(pollTimer);
    setTimeout(()=>location.reload(),4000);
  }catch(e){
    $("otaBar").style.width="0%";
    setStatus(t("ota_fail"),""); $("otaGhStatus").textContent=errText(e.message);
    $("otaInstallBtn").disabled=false;
    $("otaReinstallBtn").disabled=!otaDownloadUrl;
    $("otaCheckBtn").disabled=false;
  }
}
$("heroUpdate").onclick=ev=>{
  ev.preventDefault();
  const sec=$("secOta");
  if(sec){ sec.open=true; sec.scrollIntoView({behavior:"smooth",block:"start"}); }
};
$("otaCheckBtn").onclick=async()=>{
  $("otaCheckBtn").disabled=true;
  $("otaGhStatus").textContent=t("ota_checking");
  try{
    const res=await fetch("/api/update/check",{cache:"no-store"});
    const data=await res.json();
    if(!data.ok){
      otaDownloadUrl="";
      $("otaGhStatus").textContent=errText(data.error||"bad-json");
      setOtaInfo(data.current,"",false);
    }else{
      otaDownloadUrl=data.url||"";
      setOtaInfo(data.current,data.latest,!!data.available);
      $("otaGhStatus").textContent=data.available?t("ota_available"):t("ota_up_to_date");
    }
  }catch(e){ $("otaGhStatus").textContent=errText(e.message); }
  $("otaCheckBtn").disabled=false;
};
$("otaInstallBtn").onclick=()=>runGithubInstall(false);
$("otaReinstallBtn").onclick=()=>runGithubInstall(true);
$("otaBtn").onclick=()=>{
  const file=$("fw").files&&$("fw").files[0]; if(!file) return;
  const xhr=new XMLHttpRequest(), body=new FormData();
  body.append("firmware",file,file.name);
  $("otaProg").style.display="block"; $("otaBar").style.width="0%"; $("otaStatus").textContent=lang==="de"?"Lade hoch…":"Uploading…";
  xhr.open("POST","/api/update");
  xhr.upload.onprogress=ev=>{if(ev.lengthComputable)$("otaBar").style.width=Math.round(ev.loaded*100/ev.total)+"%"};
  xhr.onload=()=>{
    const ok=xhr.status===200;
    setStatus(ok?t("ota_ok"):t("ota_fail"),"");
    $("otaStatus").textContent=ok?t("ota_ok"):t("ota_fail");
    clearInterval(pollTimer);
    if(ok) setTimeout(()=>location.reload(),4000);
  };
  xhr.onerror=()=>{setStatus(t("ota_fail"),"");$("otaStatus").textContent=t("ota_fail")};
  xhr.send(body);
};
$("resetBtn").onclick=async()=>{
  if(!confirm(t("factory_ask"))) return;
  try{await api("/api/factory-reset",{});setStatus(t("rebooting"),"");clearInterval(pollTimer)}catch(e){setStatus(errText(e.message),"")}
};
applyLang(); refresh(); startPoll();
</script>
</body>
</html>
)PAGE";
