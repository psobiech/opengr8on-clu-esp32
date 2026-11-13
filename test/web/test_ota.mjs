// web/ota.js: application image cut out of a full image, CLU address, Basic auth.
// Run: node test/web/test_ota.mjs
import { appImage, appImageLength, basicAuth, cluBase } from "../../web/ota.js";

let failures = 0;
const check = (name, ok) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}`);
  failures += !ok;
};

/** ESP application image: header, segments of the given lengths, checksum block, optional SHA-256. */
function fakeApp(segLens, hash) {
  let len = 24;
  for (const l of segLens) len += 8 + l;
  len = (len + 16) & ~15;
  if (hash) len += 32;
  const b = new Uint8Array(len).fill(0x5a);
  const dv = new DataView(b.buffer);
  b[0] = 0xe9;
  b[1] = segLens.length;
  b[12] = 9; // chip id: ESP32-S3
  b[23] = hash ? 1 : 0;
  let p = 24;
  for (const l of segLens) {
    dv.setUint32(p, 0x3c000000, true);
    dv.setUint32(p + 4, l, true);
    p += 8 + l;
  }
  return b;
}

const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// sizes around the 16-byte checksum alignment: 24 + 8 + l + 1 ending exactly on / just past a block
for (const [segs, hash] of [[[7], true], [[8], true], [[23], false], [[100, 4, 4096], true], [[0x10000], false]]) {
  const app = fakeApp(segs, hash);
  check(`length: segments ${segs.join("+")}, hash ${hash}`, appImageLength(app) === app.length);
}

check("length: not an image", appImageLength(new Uint8Array(64)) === -1);
check("length: truncated", appImageLength(fakeApp([100], true).slice(0, 90)) === -1);
const bad = fakeApp([10], false);
bad[1] = 0;
check("length: zero segments", appImageLength(bad) === -1);
check("length: too short for a header", appImageLength(new Uint8Array([0xe9, 1])) === -1);

// full image: bootloader + partition table + app at 0x10000, followed by erased flash
const app = fakeApp([3000, 500], true);
const full = new Uint8Array(0x10000 + app.length + 4096).fill(0xff);
full.set(app, 0x10000);
const parts = [
  { label: "nvs", type: 1, subtype: 2, offset: 0x9000 },
  { label: "ota_1", type: 0, subtype: 0x11, offset: 0x30000 },
  { label: "ota_0", type: 0, subtype: 0x10, offset: 0x10000 },
];
check("full image: first app partition, exact image length", eq(appImage(full, parts), app));
check("app-only file: taken as is (trailing bytes dropped)",
      eq(appImage(new Uint8Array([...app, 1, 2, 3]), []), app));
let msg = "";
try { appImage(full, [{ type: 1, subtype: 2, offset: 0x9000 }]); } catch (e) { msg = e.message; }
check("no app partition: err_no_app", msg === "err_no_app");
msg = "";
try { appImage(new Uint8Array(full.length).fill(0xff), parts); } catch (e) { msg = e.message; }
check("erased app partition: err_no_app", msg === "err_no_app");

check("address: IP -> default port", cluBase("192.168.1.50") === "http://192.168.1.50:8080");
check("address: port, URL form, spaces, trailing slash",
      cluBase(" http://clu.lan:8081/ ") === "http://clu.lan:8081" && cluBase("10.0.0.2:80") === "http://10.0.0.2:80");
check("address: rejected (empty, https, path, bad port, IPv6)",
      [ "", "https://1.2.3.4", "1.2.3.4/ota", "1.2.3.4:0", "1.2.3.4:70000", "[::1]", "a b" ].every((s) => cluBase(s) === null));

check("auth: Basic ota:password", basicAuth("secret123") === "Basic b3RhOnNlY3JldDEyMw==");
check("auth: UTF-8 password", atob(basicAuth("zażółć1!").slice(6)) ===
      String.fromCharCode(...new TextEncoder().encode("ota:zażółć1!")));

console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failures)`);
process.exit(failures ? 1 : 0);
