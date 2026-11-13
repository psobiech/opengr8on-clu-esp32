// web/nvs.js (browser cfg image) must be byte-identical to tools/mknvs.py (ESP-IDF generator).
// Run: node test/web/test_nvs.mjs   (needs python3 with esp-idf-nvs-partition-gen)
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { buildCfgImage, validate, crc32, CFG_SIZE } from "../../web/nvs.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const python = process.env.PYTHON || "python3";
let failures = 0;
const check = (name, ok) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}`);
  failures += !ok;
};

const ARGS = { sn: "--serial", pin: "--pin", wifi_ssid: "--wifi-ssid", wifi_pass: "--wifi-pass",
               mqtt_uri: "--mqtt-uri", mqtt_user: "--mqtt-user", mqtt_pass: "--mqtt-pass" };
const dir = mkdtempSync(join(tmpdir(), "cfgnvs-"));
function reference(s) {
  const out = join(dir, "ref.bin");
  const args = [join(root, "tools", "mknvs.py"), "-o", out];
  for (const [k, flag] of Object.entries(ARGS)) {
    if (s[k] !== undefined && (s[k] !== "" || (k === "mqtt_uri" && s.mqtt_off))) args.push(flag, String(s[k]));
  }
  execFileSync(python, args, { stdio: ["ignore", "ignore", "inherit"] });
  return new Uint8Array(readFileSync(out));
}

const cases = {
  "identity only": { sn: "221010155", pin: "ABCD1234" },
  "everything": { sn: "4294967295", pin: "Q1w2E3r4", wifi_ssid: "Home,Net \"x\"", wifi_pass: "p\"a,ss word 123",
                  mqtt_uri: "mqtts://broker.example.org:8883", mqtt_user: "clu", mqtt_pass: "zażółć" },
  "MQTT off (empty URI stored)": { mqtt_uri: "", mqtt_off: true },
  "long values (multi-entry strings)": { wifi_ssid: "S".repeat(32), wifi_pass: "P".repeat(63),
                                         mqtt_uri: "ws://" + "h".repeat(200) + "/mqtt" },
};

try {
  check("crc32 = zlib.crc32", crc32(new TextEncoder().encode("123456789")) === 0xcbf43926);
  for (const [name, s] of Object.entries(cases)) {
    const mine = buildCfgImage(s);
    const ref = reference(s);
    let diff = -1;
    for (let i = 0; i < CFG_SIZE && diff < 0; i++) if (mine[i] !== ref[i]) diff = i;
    check(`${name}: identical to mknvs.py`, mine.length === ref.length && diff < 0);
    if (diff >= 0) console.log(`  first difference at 0x${diff.toString(16)}`);
  }
  check("validate: bad PIN / serial / WiFi password rejected",
        validate({ pin: "abc" }).length === 1 && validate({ sn: "0" }).length === 1 &&
        validate({ sn: "12a" }).length === 1 && validate({ wifi_pass: "short" }).length === 1 &&
        validate({ sn: "221010155", pin: "ABCD1234" }).length === 0);
  let threw = false;
  try { buildCfgImage({ pin: "x" }); } catch { threw = true; }
  check("buildCfgImage throws on invalid input", threw);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
console.log(`\n${failures ? "FAILED" : "PASSED"} (${failures} failures)`);
process.exit(failures ? 1 : 0);
