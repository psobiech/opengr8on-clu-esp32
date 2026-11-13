// opengr8on CLU web flasher: release image (full, from 0x0) + cfg settings image built in the page.
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.7.0/bundle.js";
import { buildCfgImage, validate, CFG_OFFSET, CFG_SIZE, KEYS } from "./nvs.js";
import { LANGS, apply, getLang, setLang, t } from "./i18n.js";
import { CONSOLE_BAUD, Monitor } from "./monitor.js";

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

let image = null; // {bytes, chip, parts, name}
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
      const { chip, parts } = parseImage(bytes);
      const board = (f.name.match(/opengr8on-clu-([a-z0-9]+)/) || [])[1];
      const cfg = parts.some((p) => p.label === "cfg");
      image = { bytes, chip, parts, name: f.name };
      fwInfo = { ok: true, render: () => `${BOARDS[board] || t("unknown_board")} · ${chip} · ` +
        `${(bytes.length / 1048576).toFixed(2)} MB` + (cfg ? "" : ` · ${t("fw_no_cfg")}`) };
    } catch (e) {
      fwInfo = { ok: false, render: () => (e.key ? t(e.key, e.vars) : e.message) };
    }
  }
  renderFwInfo();
});

// ---------------------------------------------------------------- settings
const form = $("cfg");
function settings() {
  const s = {};
  for (const k of KEYS) s[k] = form.elements[k].value.trim();
  s.wifi_pass = form.elements.wifi_pass.value; // passwords as typed
  s.mqtt_pass = form.elements.mqtt_pass.value;
  s.mqtt_off = form.elements.mqtt_off.checked;
  if (s.mqtt_off) s.mqtt_uri = "";
  return s;
}
const hasSettings = (s) => s.mqtt_off || KEYS.some((k) => s[k] !== "");
const writeCfg = () => $("write-cfg").checked;

/** Fields editable only with "Write new settings" checked (download stays available). */
function syncWriteCfg() {
  for (const fs of form.querySelectorAll(".cfg-fields")) fs.disabled = !writeCfg();
  $("cfg-keep-hint").hidden = writeCfg();
}
$("write-cfg").addEventListener("change", syncWriteCfg);

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
    localStorage.setItem(STORE, JSON.stringify({ sn: s.sn, wifi_ssid: s.wifi_ssid, mqtt_uri: s.mqtt_uri,
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
  if (mode === "settings" && !withCfg) throw new UiError("err_settings_keep");
  if (mode === "settings" && !hasSettings(s)) throw new UiError("err_fill_setting");
  const fwHasCfg = image && image.parts.some((p) => p.label === "cfg" && p.offset === CFG_OFFSET);
  if (mode !== "settings" && withCfg && !fwHasCfg) throw new UiError("err_fw_no_cfg");
  if (mode === "install" && !confirm(t("confirm_install"))) return null;

  logEl.textContent = "";
  const port = await navigator.serial.requestPort();
  const transport = new Transport(port, true);
  const progress = $("progress");
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
if (!("serial" in navigator)) {
  $("no-serial").hidden = false;
  btn.disabled = true;
  $("mon-open").disabled = true;
}
btn.addEventListener("click", async () => {
  btn.disabled = true;
  try {
    await monitor.stop(); // frees the port for the flasher
    const port = await flash();
    if (port) {
      monitor.clear();
      await startMonitor(port, true);
    }
  } catch (e) {
    if (e?.name !== "NotFoundError") log(`\n${t("error")}: ${e?.message || e}`); // NotFoundError = port chooser closed
  } finally {
    btn.disabled = false;
  }
});
