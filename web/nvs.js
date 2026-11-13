// Settings image for the "cfg" partition (0x12000, 12 KB), byte-identical to tools/mknvs.py
// (ESP-IDF NVS partition generator, format version 2, namespace "cfg"). Runs in the browser:
// the values (passwords included) never leave the page.

export const CFG_OFFSET = 0x12000;
export const CFG_SIZE = 0x3000;

const PAGE = 4096;
const ENTRY = 32;
const FIRST_ENTRY = 64;
const MAX_ENTRIES = 126;
const T_U8 = 0x01;
const T_U32 = 0x04;
const T_SZ = 0x21;

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** zlib.crc32(data, prev) */
export function crc32(data, prev = 0) {
  let c = ~prev >>> 0;
  for (const b of data) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

const enc = new TextEncoder();

/** Settings keys in the order tools/mknvs.py writes them. */
export const KEYS = ["sn", "pin", "wifi_ssid", "wifi_pass", "mqtt_uri", "mqtt_user", "mqtt_pass", "ota_pass"];

/**
 * Validate the settings (same rules as tools/mknvs.py). Empty / undefined = not written.
 * @returns {{key: string, vars?: object}[]} errors as message keys (web/i18n.js)
 */
export function validate(s) {
  const err = [];
  const len = (v) => enc.encode(v).length;
  if (s.sn !== undefined && s.sn !== "") {
    if (!/^\d+$/.test(String(s.sn)) || Number(s.sn) < 1 || Number(s.sn) > 0xffffffff)
      err.push({ key: "err_sn" });
  }
  if (s.pin && !/^[\x20-\x7e]{8}$/.test(s.pin))
    err.push({ key: "err_pin" });
  if (s.wifi_ssid && len(s.wifi_ssid) > 32) err.push({ key: "err_ssid" });
  if (s.wifi_pass && (len(s.wifi_pass) < 8 || len(s.wifi_pass) > 63))
    err.push({ key: "err_wifi_pass" });
  if (s.ota_pass && (len(s.ota_pass) < 8 || len(s.ota_pass) > 64))
    err.push({ key: "err_ota_pass" });
  for (const k of ["mqtt_uri", "mqtt_user", "mqtt_pass"])
    if (s[k] && len(s[k]) > 1000) err.push({ key: "err_too_long", vars: { key: k } });
  return err;
}

/**
 * Build the cfg partition image.
 * @param {object} s {sn, pin, wifi_ssid, wifi_pass, mqtt_uri, mqtt_user, mqtt_pass, ota_pass}; values that
 *   are undefined / "" are left out (mqtt_uri: pass {mqtt_uri: "", mqtt_off: true} to store "").
 * @returns {Uint8Array} CFG_SIZE bytes
 */
export function buildCfgImage(s) {
  const errors = validate(s);
  if (errors.length) throw new Error(errors.map((e) => e.key).join(" "));
  const img = new Uint8Array(CFG_SIZE).fill(0xff);
  const page = img.subarray(0, PAGE);
  const dv = new DataView(img.buffer);

  // page header: state ACTIVE, sequence 0, version 2, CRC over bytes 4..27
  dv.setUint32(0, 0xfffffffe, true);
  dv.setUint32(4, 0, true);
  page[8] = 0xfe;
  dv.setUint32(28, crc32(page.subarray(4, 28), 0xffffffff), true);

  let n = 0; // entries used
  const markWritten = () => {
    const bit = n * 2;
    page[32 + (bit >> 3)] &= ~(1 << (bit & 7));
    n++;
  };
  const header = (ns, type, span, key) => {
    if (n + span > MAX_ENTRIES - 1) throw new Error("settings do not fit in one NVS page");
    const e = new Uint8Array(ENTRY).fill(0xff);
    e[0] = ns;
    e[1] = type;
    e[2] = span;
    e[3] = 0xff; // chunk index: any
    e.fill(0, 8, 24);
    e.set(enc.encode(key), 8);
    return e;
  };
  const putEntry = (e) => {
    const crcData = new Uint8Array(28);
    crcData.set(e.subarray(0, 4), 0);
    crcData.set(e.subarray(8, 32), 4);
    new DataView(e.buffer).setUint32(4, crc32(crcData, 0xffffffff), true);
    page.set(e, FIRST_ENTRY + n * ENTRY);
    markWritten();
  };

  // namespace "cfg" = index 1
  let e = header(0, T_U8, 1, "cfg");
  e[24] = 1;
  putEntry(e);

  for (const key of KEYS) {
    let v = s[key];
    const keepEmpty = key === "mqtt_uri" && s.mqtt_off;
    if (v === undefined || v === null || (v === "" && !keepEmpty)) continue;
    if (key === "sn") {
      e = header(1, T_U32, 1, key);
      new DataView(e.buffer).setUint32(24, Number(v), true);
      putEntry(e);
      continue;
    }
    const data = enc.encode(String(v) + "\0");
    const dataEntries = (data.length + 31) >> 5;
    e = header(1, T_SZ, dataEntries + 1, key);
    const edv = new DataView(e.buffer);
    edv.setUint16(24, data.length, true);
    edv.setUint32(28, crc32(data, 0xffffffff), true);
    putEntry(e);
    page.set(data, FIRST_ENTRY + n * ENTRY);
    for (let i = 0; i < dataEntries; i++) markWritten();
  }
  return img;
}
