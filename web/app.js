// opengr8on CLU web flasher: release image (full, from 0x0) + cfg settings image built in the page.
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.7.0/bundle.js";
import { buildCfgImage, validate, CFG_OFFSET, CFG_SIZE, KEYS } from "./nvs.js";
import { LANGS, apply, getLang, setLang, t } from "./i18n.js";
import { CONSOLE_BAUD, Monitor } from "./monitor.js";
import { appImage, appImageLength, basicAuth, cluBase } from "./ota.js";

const $ = (id) => document.getElementById(id);
const BAUD = 460800;
const CHIPS = { 0: "ESP32", 9: "ESP32-S3" }; // esp_image_header_t chip_id
const BOARDS = {
  waveshare: "Waveshare ESP32-S3-RS485-CAN",
  gate: "Grenton GATE",
  f8: "KinCony F8",
  a8v3: "KinCony KC868-A8v3",
};
const STORE = "opengr8on-flasher"; // non-secret form fields, per browser

let image = null; // {bytes, chip, parts, name, appOnly}
let fwInfo = null; // last firmware check: {ok, render()} (re-rendered on a language change)

/** Error with a translatable message. */
class UiError extends Error {
  constructor(key, vars) {
    super(t(key, vars));
    this.key = key;
    this.vars = vars;
  }
}

// ---------------------------------------------------------------- language
apply();
const langSel = $("lang");
for (const [code, name] of Object.entries(LANGS)) langSel.add(new Option(name, code, false, code === getLang()));
langSel.addEventListener("change", () => {
  setLang(langSel.value);
  renderFwInfo();
  checkSettings();
});

// ---------------------------------------------------------------- image
/** Partition table at 0x8000: [{label, type, subtype, offset, size}] */
function partitions(buf, at = 0x8000) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const out = [];
  for (let p = at; p + 32 <= buf.length && p < at + 0xc00; p += 32) {
    if (buf[p] !== 0xaa || buf[p + 1] !== 0x50) break; // 0xEBEB = MD5 entry, 0xFF = end
    const label = new TextDecoder().decode(buf.subarray(p + 12, p + 28)).replace(/\0.*$/s, "");
    out.push({ label, type: buf[p + 2], subtype: buf[p + 3], offset: dv.getUint32(p + 4, true),
               size: dv.getUint32(p + 8, true) });
  }
  return out;
}

function parseImage(bytes) {
  // application-only image (release -ota.bin / opengr8on_clu_esp32.bin): no partition table, usable for OTA only
  if (bytes[0] === 0xe9 && !partitions(bytes).length && appImageLength(bytes) > 0) {
    const chip = CHIPS[bytes[12] | (bytes[13] << 8)];
    if (!chip) throw new UiError("err_chip_unsupported");
    return { chip, parts: [], appOnly: true };
  }
  const boot = bytes[0] === 0xe9 ? 0 : bytes[0x1000] === 0xe9 ? 0x1000 : -1;
  if (boot < 0) throw new UiError("err_not_full");
  const chip = CHIPS[bytes[boot + 12] | (bytes[boot + 13] << 8)];
  if (!chip) throw new UiError("err_chip_unsupported");
  const parts = partitions(bytes);
  if (!parts.length) throw new UiError("err_no_pt");
  return { chip, parts };
}

/**
 * Regions of the image to write: everything except data partitions other than otadata
 * (nvs = runtime state, phy_init, cfg = settings; storage lies past the image end).
 */
function segments(bytes, parts) {
  const skip = parts.filter((p) => p.type === 0x01 && p.subtype !== 0x00)
    .map((p) => [p.offset, p.offset + p.size]).sort((a, b) => a[0] - b[0]);
  const out = [];
  let at = 0;
  for (const [s, e] of skip) {
    if (s > at) out.push([at, Math.min(s, bytes.length)]);
    at = Math.max(at, e);
  }
  if (at < bytes.length) out.push([at, bytes.length]);
  return out.filter(([s, e]) => e > s).map(([s, e]) => ({ address: s, data: bytes.slice(s, e) }));
}

function renderFwInfo() {
  const info = $("fw-info");
  info.className = fwInfo ? (fwInfo.ok ? "info ok" : "warn") : "info";
  info.textContent = fwInfo ? fwInfo.render() : "";
}

$("fw").addEventListener("change", async (ev) => {
  const f = ev.target.files[0];
  image = null;
  fwInfo = null;
  if (f) {
    try {
      const bytes = new Uint8Array(await f.arrayBuffer());
      const { chip, parts, appOnly } = parseImage(bytes);
      const board = (f.name.match(/opengr8on-clu-([a-z0-9]+)/) || [])[1];
      const cfg = parts.some((p) => p.label === "cfg");
      image = { bytes, chip, parts, name: f.name, appOnly: !!appOnly };
      fwInfo = { ok: true, render: () => `${BOARDS[board] || t("unknown_board")} · ${chip} · ` +
        `${(bytes.length / 1048576).toFixed(2)} MB` +
        (appOnly ? ` · ${t("app_only")}` : cfg ? "" : ` · ${t("fw_no_cfg")}`) };
    } catch (e) {
      fwInfo = { ok: false, render: () => (e.key ? t(e.key, e.vars) : e.message) };
    }
  }
  renderFwInfo();
  syncMode(); // an application-only image allows the network update only
});

// ---------------------------------------------------------------- settings
const form = $("cfg");
function settings() {
  const s = {};
  for (const k of KEYS) s[k] = form.elements[k].value.trim();
  s.wifi_pass = form.elements.wifi_pass.value; // passwords as typed
  s.mqtt_pass = form.elements.mqtt_pass.value;
  s.ota_pass = form.elements.ota_pass.value;
  s.mqtt_off = form.elements.mqtt_off.checked;
  if (s.mqtt_off) s.mqtt_uri = "";
  return s;
}
const hasSettings = (s) => s.mqtt_off || KEYS.some((k) => s[k] !== "");
const writeCfg = () => $("write-cfg").checked;

const otaMode = () => document.querySelector("input[name=mode]:checked")?.value === "ota";

const modeRadio = (m) => document.querySelector(`input[name=mode][value=${m}]`);

/**
 * Which modes are possible: an application-only image can only go over the network (OTA is the
 * only mode; new settings are unticked, their values stay in the form), and the network update
 * cannot write settings (no OTA with new settings). A mode that becomes impossible is replaced
 * (OTA for an application image, else settings only).
 */
function syncModeAvailability() {
  const appOnly = !!image?.appOnly;
  if (appOnly) $("write-cfg").checked = false;
  for (const m of ["install", "update", "settings"]) modeRadio(m).disabled = appOnly;
  modeRadio("ota").disabled = writeCfg();
  $("ota-mode-hint").hidden = !writeCfg();
  const cur = document.querySelector("input[name=mode]:checked");
  if (cur.disabled) modeRadio(appOnly ? "ota" : "settings").checked = true;
}

/**
 * Fields editable only with "Write new settings" checked (download stays available). The network
 * update cannot write settings: in OTA mode step 2 is off, and with new settings OTA is off.
 */
function syncWriteCfg() {
  syncModeAvailability();
  const ota = otaMode();
  const write = writeCfg() && !ota;
  $("write-cfg").disabled = ota;
  $("download").disabled = ota;
  for (const fs of form.querySelectorAll(".cfg-fields")) fs.disabled = !write;
  $("cfg-keep-hint").hidden = write || ota;
  $("cfg-ota-hint").hidden = !ota;
  $("s2").classList.toggle("off", ota);
}
$("write-cfg").addEventListener("change", () => syncMode());

function checkSettings() {
  const errors = validate(settings());
  $("cfg-errors").textContent = errors.map((e) => t(e.key, e.vars)).join(" ");
  return errors.length === 0;
}

try { // restore non-secret fields
  const saved = JSON.parse(localStorage.getItem(STORE) || "{}");
  for (const k of ["sn", "wifi_ssid", "mqtt_uri", "mqtt_user"]) if (saved[k]) form.elements[k].value = saved[k];
} catch { /* storage unavailable */ }
syncWriteCfg();
form.addEventListener("input", () => {
  form.elements.mqtt_uri.disabled = form.elements.mqtt_off.checked;
  checkSettings();
  try {
    const s = settings();
    const saved = JSON.parse(localStorage.getItem(STORE) || "{}");
    localStorage.setItem(STORE, JSON.stringify({ ...saved, sn: s.sn, wifi_ssid: s.wifi_ssid, mqtt_uri: s.mqtt_uri,
                                                 mqtt_user: s.mqtt_user }));
  } catch { /* storage unavailable */ }
});

$("download").addEventListener("click", () => {
  if (!checkSettings()) return;
  const s = settings();
  if (!hasSettings(s)) {
    $("cfg-errors").textContent = t("need_setting");
    return;
  }
  const url = URL.createObjectURL(new Blob([buildCfgImage(s)], { type: "application/octet-stream" }));
  const a = Object.assign(document.createElement("a"), { href: url, download: "cfg.bin" });
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

// ---------------------------------------------------------------- flashing
const logEl = $("log");
const log = (s) => {
  logEl.textContent += s + "\n";
  logEl.scrollTop = logEl.scrollHeight;
};
const terminal = {
  clean() {},
  writeLine: (s) => log(s),
  write: (s) => {
    logEl.textContent += s;
  },
};

/** Flash per the form; resolves to the (closed) port on success, null when cancelled. */
async function flash() {
  const mode = document.querySelector("input[name=mode]:checked").value;
  const s = settings();
  const withCfg = writeCfg(); // else: keep the device's cfg partition
  if (withCfg && !checkSettings()) return null;
  if (mode !== "settings" && !image) throw new UiError("err_select_fw");
  if (mode !== "settings" && image.appOnly) throw new UiError("err_app_only");
  if (mode === "settings" && !withCfg) throw new UiError("err_settings_keep");
  if (mode === "settings" && !hasSettings(s)) throw new UiError("err_fill_setting");
  const fwHasCfg = image && image.parts.some((p) => p.label === "cfg" && p.offset === CFG_OFFSET);
  if (mode !== "settings" && withCfg && !fwHasCfg) throw new UiError("err_fw_no_cfg");
  if (mode === "install" && !confirm(t("confirm_install"))) return null;

  logEl.textContent = "";
  const port = await navigator.serial.requestPort();
  const transport = new Transport(port, true);
  try {
    const loader = new ESPLoader({ transport, baudrate: BAUD, terminal });
    await loader.main();
    const chip = loader.chip.CHIP_NAME;
    if (image && mode !== "settings" && chip !== image.chip)
      throw new UiError("err_wrong_chip", { chip, image: image.chip });
    // settings only: the device must already run firmware with the cfg partition;
    // install keeping the settings: read them before the erase, write them back after
    let kept = null;
    if (mode === "settings" || (mode === "install" && !withCfg)) {
      const pt = partitions(await loader.readFlash(0x8000, 0xc00), 0);
      const devCfg = pt.some((p) => p.label === "cfg" && p.offset === CFG_OFFSET && p.size >= CFG_SIZE);
      if (mode === "settings" && !devCfg) throw new UiError("err_device_no_cfg");
      if (mode === "install") {
        const cur = devCfg && fwHasCfg ? await loader.readFlash(CFG_OFFSET, CFG_SIZE) : null;
        kept = cur && cur.some((b) => b !== 0xff) ? cur : null;
        log(t(kept ? "log_cfg_kept" : "log_cfg_none"));
      }
    } else if (mode === "update" && !withCfg) {
      log(t("log_cfg_kept"));
    }

    const files = mode === "settings" ? [] : segments(image.bytes, image.parts);
    if (withCfg) files.push({ address: CFG_OFFSET, data: buildCfgImage(s) });
    else if (kept) files.push({ address: CFG_OFFSET, data: kept });
    const total = files.reduce((n, f) => n + f.data.length, 0);
    const done = new Array(files.length).fill(0);
    progress.hidden = false;
    progress.value = 0;
    for (const f of files) log(`0x${f.address.toString(16).padStart(5, "0")}  ${f.data.length} bytes`);
    await loader.writeFlash({
      fileArray: files,
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: mode === "install",
      compress: true,
      reportProgress: (i, written) => {
        done[i] = written;
        progress.value = done.reduce((a, b) => a + b, 0) / total;
      },
    });
    progress.value = 1;
    log(`\n${t("done", { mode: t(`mode_${mode}`) })}`);
    return port; // the monitor resets the chip into the firmware
  } finally {
    await transport.disconnect().catch(() => {});
  }
}

// ---------------------------------------------------------------- network update (OTA)
const otaHost = $("ota-host");
const progress = $("progress");
let otaBlob = null; // object URL of the application image offered by the fallback

function otaFallback(base, app) {
  if (otaBlob) URL.revokeObjectURL(otaBlob);
  otaBlob = URL.createObjectURL(new Blob([app], { type: "application/octet-stream" }));
  $("ota-open").href = `${base}/`;
  $("ota-save").href = otaBlob;
  $("ota-save").download = image.appOnly ? image.name : image.name.replace(/(\.bin)?$/i, "-ota.bin");
  $("ota-fallback").hidden = false;
}

/**
 * Upload the application image to a running CLU (POST /ota). The page is HTTPS and the CLU plain
 * HTTP on the LAN: only browsers with local network access (Chrome, after the user allows it) let
 * the request through; otherwise the CLU's own upload page is offered.
 */
async function otaFlash() {
  if (!image) throw new UiError("err_select_fw");
  const base = cluBase(otaHost.value);
  if (!base) throw new UiError("err_clu_addr");
  const pw = $("ota-pw").value;
  const pwLen = new TextEncoder().encode(pw).length;
  if (pwLen < 8 || pwLen > 64) throw new UiError("err_ota_pass");
  let app;
  try {
    app = appImage(image.bytes, image.parts);
  } catch (e) {
    throw new UiError(e.message);
  }

  logEl.textContent = "";
  $("ota-fallback").hidden = true;
  const auth = basicAuth(pw);
  const post = (body) => fetch(`${base}/ota`, {
    method: "POST",
    headers: { Authorization: auth, "Content-Type": "application/octet-stream" },
    body,
    cache: "no-store",
    targetAddressSpace: "local", // Chrome local network access; ignored elsewhere
  });
  progress.hidden = false;
  progress.removeAttribute("value"); // fetch has no upload progress: indeterminate
  try {
    log(t("log_ota_check", { base }));
    let r;
    try {
      r = await post(new Uint8Array(0)); // password check: 411 = right, 401 = wrong
    } catch {
      log(t("log_ota_blocked"));
      otaFallback(base, app);
      return;
    }
    if (r.status === 401) throw new UiError("err_ota_auth");
    if (r.status !== 411) throw new UiError("err_ota_unexpected", { status: r.status });
    log(t("log_ota_upload", { size: (app.length / 1048576).toFixed(2) }));
    try {
      r = await post(app);
    } catch {
      throw new UiError("err_ota_lost");
    }
    const text = (await r.text().catch(() => "")).trim();
    if (!r.ok) throw new UiError("err_ota_status", { status: r.status, text });
    log(`\n${t("done_ota", { base })}`);
  } finally {
    progress.hidden = true;
    progress.value = 0;
  }
}

// ---------------------------------------------------------------- UART log
const monEl = $("mon-log");
const monitor = new Monitor(monEl, {
  note: (k) => `--- ${t(`mon_${k}`)} ---`,
  onState: (on) => {
    $("mon").hidden = false;
    $("mon-stop").disabled = !on;
    $("mon-reset").disabled = !on;
    $("mon-state").textContent = t(on ? "mon_on" : "mon_off", { baud: CONSOLE_BAUD });
  },
});

async function startMonitor(port, reset) {
  try {
    await monitor.start(port, { reset });
    monEl.scrollIntoView({ behavior: "smooth", block: "nearest" });
  } catch (e) {
    log(`\n${t("error")}: ${t("mon_fail")} (${e?.message || e})`);
  }
}

$("mon-open").addEventListener("click", async () => {
  await monitor.stop();
  try {
    await startMonitor(await navigator.serial.requestPort(), false);
  } catch { /* port chooser closed */ }
});
$("mon-reset").addEventListener("click", () => monitor.reset().catch(() => {}));
$("mon-stop").addEventListener("click", () => monitor.stop());
$("mon-clear").addEventListener("click", () => monitor.clear());
$("mon-save").addEventListener("click", () => {
  const url = URL.createObjectURL(new Blob([monitor.text()], { type: "text/plain" }));
  const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
  Object.assign(document.createElement("a"), { href: url, download: `clu-uart-${stamp}.log` }).click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

const btn = $("flash");
const hasSerial = "serial" in navigator;
const mode = () => document.querySelector("input[name=mode]:checked").value;
/** Serial modes need Web Serial; the network update does not. */
function syncMode() {
  syncWriteCfg();
  const ota = mode() === "ota";
  $("ota-net").hidden = !ota;
  $("boot-hint").hidden = ota;
  $("ota-fallback").hidden ||= !ota;
  btn.dataset.i18n = ota ? "ota_btn" : "flash_btn";
  btn.textContent = t(btn.dataset.i18n);
  btn.disabled = !ota && !hasSerial;
  // the UART log needs the board on USB: off (and closed) for the network update
  $("mon-open").disabled = ota || !hasSerial;
  if (ota && !$("mon").hidden) monitor.stop().finally(() => { $("mon").hidden = mode() === "ota" || $("mon").hidden; });
}
for (const r of document.querySelectorAll("input[name=mode]")) r.addEventListener("change", syncMode);
if (!hasSerial) $("no-serial").hidden = false;
try {
  otaHost.value = JSON.parse(localStorage.getItem(STORE) || "{}").ota_host || "";
} catch { /* storage unavailable */ }
otaHost.addEventListener("change", () => {
  try {
    const saved = JSON.parse(localStorage.getItem(STORE) || "{}");
    localStorage.setItem(STORE, JSON.stringify({ ...saved, ota_host: otaHost.value.trim() }));
  } catch { /* storage unavailable */ }
});
syncMode();

btn.addEventListener("click", async () => {
  btn.disabled = true;
  try {
    if (mode() === "ota") {
      await otaFlash();
      return;
    }
    await monitor.stop(); // frees the port for the flasher
    const port = await flash();
    if (port) {
      monitor.clear();
      await startMonitor(port, true);
    }
  } catch (e) {
    if (e?.name !== "NotFoundError") log(`\n${t("error")}: ${e?.message || e}`); // NotFoundError = port chooser closed
  } finally {
    syncMode();
  }
});
