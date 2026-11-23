// Network update (OTA) helpers for the web flasher: the application image for POST /ota (cut out
// of the full release image, or an application-only .bin as is), the CLU address and HTTP Basic
// auth. No DOM, no network: tested with node (test/web/test_ota.mjs).

const IMAGE_MAGIC = 0xe9;
const HEADER_LEN = 24; // esp_image_header_t (8) + extended header (16)
const SEG_HEADER_LEN = 8; // esp_image_segment_header_t: load address, length
const HASH_LEN = 32; // SHA-256 appended when header byte 23 (hash_appended) is 1
export const DEFAULT_PORT = 8080;

/**
 * Length of the ESP application image starting at @p off: header, segments, checksum (pads to a
 * 16-byte boundary) and the optional SHA-256.
 * @returns {number} bytes, or -1 when there is no valid image there
 */
export function appImageLength(bytes, off = 0) {
  if (off + HEADER_LEN > bytes.length || bytes[off] !== IMAGE_MAGIC) return -1;
  const dataView = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const segs = bytes[off + 1];
  if (segs < 1 || segs > 16) return -1; // ESP_IMAGE_MAX_SEGMENTS
  let position = off + HEADER_LEN;
  for (let i = 0; i < segs; i++) {
    if (position + SEG_HEADER_LEN > bytes.length) return -1;
    position += SEG_HEADER_LEN + dataView.getUint32(position + 4, true);
  }
  position = (position + 16) & ~15; // checksum byte ends the 16-byte block (esptool align_file_position + 1)
  if (bytes[off + 23] === 1) position += HASH_LEN;
  return position <= bytes.length ? position - off : -1;
}

/**
 * The application image to upload.
 * @param {Uint8Array} bytes selected file
 * @param {{type:number, subtype:number, offset:number}[]} parts its partition table ([] = app-only file)
 * @returns {Uint8Array}
 * @throws {Error} message = i18n key (err_no_app)
 */
export function appImage(bytes, parts) {
  // full image: the first app partition (ota_0, else factory); app-only file: offset 0
  const app = parts.length
    ? parts.filter((partition) => partition.type === 0x00).sort((left, right) => left.offset - right.offset)[0]
    : { offset: 0 };
  const len = app ? appImageLength(bytes, app.offset) : -1;
  if (len < 0) throw new Error("err_no_app");
  return bytes.slice(app.offset, app.offset + len);
}

/**
 * Base URL of the CLU's OTA server from what the user typed: "192.168.1.50", "clu.lan:8081",
 * "http://192.168.1.50/"...
 * @returns {string|null} "http://host:port", null when not a host name / IPv4 (+ port)
 */
export function cluBase(input) {
  const address = String(input).trim().replace(/^http:\/\//i, "").replace(/\/+$/, "");
  const match = /^([A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?)(?::(\d{1,5}))?$/.exec(address);
  if (!match) return null;
  const port = match[2] ? Number(match[2]) : DEFAULT_PORT;
  if (port < 1 || port > 65535) return null;
  return `http://${match[1]}:${port}`;
}

/** "Basic base64(ota:password)", UTF-8 (the CLU ignores the user name). */
export function basicAuth(password) {
  const encoded = new TextEncoder().encode(`ota:${password}`);
  let binary = "";
  for (const c of encoded) binary += String.fromCharCode(c);
  return `Basic ${btoa(binary)}`;
}
