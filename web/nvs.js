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
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let crc = n;
    for (let k = 0; k < 8; k++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    table[n] = crc >>> 0;
  }
  return table;
})();

/** zlib.crc32(data, prev) */
export function crc32(data, prev = 0) {
  let crc = ~prev >>> 0;
  for (const b of data) crc = CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

const enc = new TextEncoder();

/** Settings keys in the order tools/mknvs.py writes them. */
export const KEYS = ["sn", "pin", "wifi_ssid", "wifi_pass", "mqtt_uri", "mqtt_user", "mqtt_pass", "ota_pass"];

/**
 * Validate the settings (same rules as tools/mknvs.py). Empty / undefined = not written.
 * @returns {{key: string, vars?: object}[]} errors as message keys (web/i18n.js)
 */
export function validate(values) {
  const err = [];
  const len = (text) => enc.encode(text).length;
  if (values.sn !== undefined && values.sn !== "") {
    if (!/^\d+$/.test(String(values.sn)) || Number(values.sn) < 1 || Number(values.sn) > 0xffffffff)
      err.push({ key: "err_sn" });
  }
  if (values.pin && !/^[\x20-\x7e]{8}$/.test(values.pin))
    err.push({ key: "err_pin" });
  if (values.wifi_ssid && len(values.wifi_ssid) > 32) err.push({ key: "err_ssid" });
  if (values.wifi_pass && (len(values.wifi_pass) < 8 || len(values.wifi_pass) > 63))
    err.push({ key: "err_wifi_pass" });
  if (values.ota_pass && (len(values.ota_pass) < 8 || len(values.ota_pass) > 64))
    err.push({ key: "err_ota_pass" });
  for (const k of ["mqtt_uri", "mqtt_user", "mqtt_pass"])
    if (values[k] && len(values[k]) > 1000) err.push({ key: "err_too_long", vars: { key: k } });
  return err;
}

/**
 * Build the cfg partition image.
 * @param {object} values {sn, pin, wifi_ssid, wifi_pass, mqtt_uri, mqtt_user, mqtt_pass, ota_pass}; values that
 *   are undefined / "" are left out (mqtt_uri: pass {mqtt_uri: "", mqtt_off: true} to store "").
 * @returns {Uint8Array} CFG_SIZE bytes
 */
export function buildCfgImage(values) {
  const errors = validate(values);
  if (errors.length) throw new Error(errors.map((error) => error.key).join(" "));
  const img = new Uint8Array(CFG_SIZE).fill(0xff);
  const page = img.subarray(0, PAGE);
  const dataView = new DataView(img.buffer);

  // page header: state ACTIVE, sequence 0, version 2, CRC over bytes 4..27
  dataView.setUint32(0, 0xfffffffe, true);
  dataView.setUint32(4, 0, true);
  page[8] = 0xfe;
  dataView.setUint32(28, crc32(page.subarray(4, 28), 0xffffffff), true);

  let entryCount = 0; // entries used
  const markWritten = () => {
    const bit = entryCount * 2;
    page[32 + (bit >> 3)] &= ~(1 << (bit & 7));
    entryCount++;
  };
  const header = (namespaceIndex, type, span, key) => {
    if (entryCount + span > MAX_ENTRIES - 1) throw new Error("settings do not fit in one NVS page");
    const entry = new Uint8Array(ENTRY).fill(0xff);
    entry[0] = namespaceIndex;
    entry[1] = type;
    entry[2] = span;
    entry[3] = 0xff; // chunk index: any
    entry.fill(0, 8, 24);
    entry.set(enc.encode(key), 8);
    return entry;
  };
  const putEntry = (entry) => {
    const crcData = new Uint8Array(28);
    crcData.set(entry.subarray(0, 4), 0);
    crcData.set(entry.subarray(8, 32), 4);
    new DataView(entry.buffer).setUint32(4, crc32(crcData, 0xffffffff), true);
    page.set(entry, FIRST_ENTRY + entryCount * ENTRY);
    markWritten();
  };

  // namespace "cfg" = index 1
  let entry = header(0, T_U8, 1, "cfg");
  entry[24] = 1;
  putEntry(entry);

  for (const key of KEYS) {
    let value = values[key];
    const keepEmpty = key === "mqtt_uri" && values.mqtt_off;
    if (value === undefined || value === null || (value === "" && !keepEmpty)) continue;
    if (key === "sn") {
      entry = header(1, T_U32, 1, key);
      new DataView(entry.buffer).setUint32(24, Number(value), true);
      putEntry(entry);
      continue;
    }
    const data = enc.encode(String(value) + "\0");
    const dataEntries = (data.length + 31) >> 5;
    entry = header(1, T_SZ, dataEntries + 1, key);
    const entryView = new DataView(entry.buffer);
    entryView.setUint16(24, data.length, true);
    entryView.setUint32(28, crc32(data, 0xffffffff), true);
    putEntry(entry);
    page.set(data, FIRST_ENTRY + entryCount * ENTRY);
    for (let i = 0; i < dataEntries; i++) markWritten();
  }
  return img;
}
