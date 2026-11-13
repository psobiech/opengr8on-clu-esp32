// Page texts. data-i18n="key" sets textContent, data-i18n-html="key" innerHTML (static, trusted
// strings below), data-i18n-placeholder="key" the placeholder. Polish is the default.

export const LANGS = { pl: "Polski", en: "English" };
export const DEFAULT_LANG = "pl";
const STORE = "opengr8on-flasher-lang";

const RELEASES = "https://github.com/psobiech/opengr8on-clu-esp32/releases";

const T = {
  pl: {
    title: "Flasher opengr8on CLU",
    intro: "Instalacja i aktualizacja firmware ESP32 CLU oraz zapis jego ustawień przez USB, w Chrome lub Edge na komputerze. Wszystko dzieje się w tej stronie: ustawienia (także hasła) nie są nigdzie wysyłane.",
    no_serial: "Ta przeglądarka nie obsługuje Web Serial. Użyj Chrome lub Edge na komputerze albo wgraj firmware z linii poleceń (zob. README).",
    s1_title: "Firmware",
    s1_text: `Pobierz obraz dla swojej płytki ze <a href="${RELEASES}" target="_blank" rel="noopener">strony wydań</a> (<code>opengr8on-clu-&lt;płytka&gt;-&lt;wersja&gt;.bin</code>) i wybierz go tutaj. Niepotrzebny przy „Tylko ustawienia”.`,
    th_board: "Płytka", th_file: "Plik", th_net: "Sieć",
    net_eth: "Ethernet (DHCP)",
    fw_label: "Obraz firmware",
    s2_title: "Ustawienia",
    s2_text: "Zapisywane w partycji <code>cfg</code>, osobno od stanu urządzenia. Puste pola nie są zapisywane: CLU zachowuje swój numer seryjny i PIN (losowe przy pierwszym starcie) i używa wbudowanych ustawień WiFi / MQTT. Nowy numer seryjny lub PIN resetuje klucze projektu (Object Manager musi ponownie ustawić klucz).",
    cfg_write: "<b>Zapisz nowe ustawienia</b> — zastępują ustawienia na urządzeniu",
    cfg_keep_hint: "Bez zaznaczenia ustawienia na urządzeniu zostają (przy instalacji są odczytywane przed kasowaniem i wgrywane z powrotem).",
    err_settings_keep: "„Tylko ustawienia” wymaga zaznaczenia „Zapisz nowe ustawienia” (krok 2).",
    log_cfg_kept: "Ustawienia z urządzenia zostaną zachowane.",
    log_cfg_none: "Na urządzeniu nie ma ustawień do zachowania.",
    lg_identity: "Tożsamość CLU",
    l_sn: "Numer seryjny", l_pin: "PIN / klucz prywatny", ph_pin: "8 znaków",
    lg_wifi: "WiFi", wifi_only: "(tylko płytki z WiFi)",
    l_ssid: "SSID", l_pass: "Hasło",
    lg_mqtt: "MQTT / Home Assistant",
    l_uri: "Adres brokera", l_user: "Użytkownik",
    mqtt_off: "MQTT wyłączone (nawet jeśli wbudowane)",
    download: "Pobierz cfg.bin (opcjonalne)",
    download_hint: "do <code>esptool write-flash 0x12000 cfg.bin</code>",
    s3_title: "Wgrywanie",
    m_install: "<b>Instalacja</b> — kasuje cały flash i wgrywa firmware",
    m_update: "<b>Aktualizacja</b> — wgrywa firmware, zachowuje stan i dysk a:\\",
    m_settings: "<b>Tylko ustawienia</b> — zapisuje ustawienia, nic więcej",
    boot_hint: "Podłącz płytkę przez USB. Jeśli nie chce się połączyć: przytrzymaj BOOT, naciśnij i puść RESET, puść BOOT i spróbuj ponownie.",
    flash_btn: "Połącz i wgraj",
    lang: "Język",
    // messages
    unknown_board: "Nieznana płytka",
    fw_no_cfg: "ten firmware nie ma partycji cfg: nie da się zapisać ustawień",
    err_not_full: "to nie jest pełny obraz firmware (brak bootloadera pod 0x0 / 0x1000)",
    err_chip_unsupported: "nieobsługiwany układ w obrazie",
    err_no_pt: "brak tablicy partycji pod 0x8000: użyj pełnego obrazu ze strony wydań",
    need_setting: "Wypełnij co najmniej jedno ustawienie.",
    err_select_fw: "Najpierw wybierz obraz firmware (krok 1).",
    err_fill_setting: "Wypełnij co najmniej jedno ustawienie (krok 2).",
    err_fw_no_cfg: "Ten firmware nie ma partycji cfg pod 0x12000: wyczyść ustawienia albo użyj nowszego wydania.",
    confirm_install: "Instalacja kasuje cały flash: projekt, klucze i dysk a:\\ na urządzeniu. Kontynuować?",
    err_wrong_chip: "Podłączony układ to {chip}, a firmware jest dla {image}: zła płytka albo plik.",
    err_device_no_cfg: "Firmware na urządzeniu nie ma partycji cfg: najpierw je zaktualizuj albo zainstaluj.",
    done: "Gotowe ({mode}). CLU uruchamia się ponownie; numer seryjny i PIN są w logu UART poniżej.",
    mon_open: "Pokaż log UART",
    mon_title: "Log UART",
    mon_on: "połączono, {baud} bodów",
    mon_off: "rozłączono",
    mon_reset: "Reset",
    mon_clear: "Wyczyść",
    mon_save: "Zapisz",
    mon_stop: "Rozłącz",
    mon_lost: "port zniknął, czekam na ponowne podłączenie",
    mon_back: "połączono ponownie",
    mon_fail: "nie udało się otworzyć logu UART; naciśnij RESET na płytce",
    mode_install: "instalacja", mode_update: "aktualizacja", mode_settings: "tylko ustawienia",
    error: "Błąd",
    err_sn: "Numer seryjny musi być liczbą dziesiętną 1..4294967295.",
    err_pin: "PIN / klucz prywatny musi mieć dokładnie 8 drukowalnych znaków ASCII.",
    err_ssid: "SSID WiFi musi mieć 1..32 bajty.",
    err_wifi_pass: "Hasło WiFi musi mieć 8..63 bajty.",
    err_too_long: "Pole {key} jest za długie.",
  },
  en: {
    title: "opengr8on CLU flasher",
    intro: "Install or update the ESP32 CLU firmware and write its settings over USB, from Chrome or Edge on a desktop computer. Everything happens in this page: the settings (passwords included) are never sent anywhere.",
    no_serial: "This browser has no Web Serial. Use Chrome or Edge on a desktop computer, or flash from the command line (see the README).",
    s1_title: "Firmware",
    s1_text: `Download the image for your board from the <a href="${RELEASES}" target="_blank" rel="noopener">releases page</a> (<code>opengr8on-clu-&lt;board&gt;-&lt;version&gt;.bin</code>) and select it here. Not needed for "Settings only".`,
    th_board: "Board", th_file: "File", th_net: "Network",
    net_eth: "Ethernet (DHCP)",
    fw_label: "Firmware image",
    s2_title: "Settings",
    s2_text: "Stored in the <code>cfg</code> partition, separate from the runtime state. Empty fields are not stored: the CLU keeps its serial / PIN (random on first boot) and uses its built-in WiFi / MQTT defaults. A new serial or PIN resets the project keys (Object Manager needs the key again).",
    cfg_write: "<b>Write new settings</b> — they replace the settings on the device",
    cfg_keep_hint: "Unchecked, the settings on the device stay (an install reads them before erasing and writes them back).",
    err_settings_keep: "\"Settings only\" needs \"Write new settings\" checked (step 2).",
    log_cfg_kept: "The settings on the device are kept.",
    log_cfg_none: "No settings on the device to keep.",
    lg_identity: "CLU identity",
    l_sn: "Serial number", l_pin: "PIN / private key", ph_pin: "8 characters",
    lg_wifi: "WiFi", wifi_only: "(WiFi boards only)",
    l_ssid: "SSID", l_pass: "Password",
    lg_mqtt: "MQTT / Home Assistant",
    l_uri: "Broker URI", l_user: "Username",
    mqtt_off: "MQTT off (even if built in)",
    download: "Download cfg.bin (optional)",
    download_hint: "for <code>esptool write-flash 0x12000 cfg.bin</code>",
    s3_title: "Flash",
    m_install: "<b>Install</b> — erase the whole flash and write the firmware",
    m_update: "<b>Update</b> — write the firmware, keep the state and the a:\\ drive",
    m_settings: "<b>Settings only</b> — write the settings, nothing else",
    boot_hint: "Connect the board over USB. If it does not connect, hold BOOT, press and release RESET, then release BOOT and try again.",
    flash_btn: "Connect and flash",
    lang: "Language",
    unknown_board: "Unknown board",
    fw_no_cfg: "this firmware has no cfg partition: settings cannot be written",
    err_not_full: "not a full firmware image (no bootloader at 0x0 / 0x1000)",
    err_chip_unsupported: "unsupported chip in the image",
    err_no_pt: "no partition table at 0x8000: use the full image from the releases page",
    need_setting: "Fill in at least one setting.",
    err_select_fw: "Select the firmware image first (step 1).",
    err_fill_setting: "Fill in at least one setting (step 2).",
    err_fw_no_cfg: "This firmware has no cfg partition at 0x12000; clear the settings or use a newer release.",
    confirm_install: "Install erases the whole flash: project, keys and the a:\\ drive on the device. Continue?",
    err_wrong_chip: "Connected chip is {chip}, the firmware is for {image}: wrong board or file.",
    err_device_no_cfg: "The firmware on the device has no cfg partition: update or install it first.",
    done: "Done ({mode}). The CLU restarts; its serial number and PIN are in the UART log below.",
    mon_open: "Show UART log",
    mon_title: "UART log",
    mon_on: "connected, {baud} baud",
    mon_off: "disconnected",
    mon_reset: "Reset",
    mon_clear: "Clear",
    mon_save: "Save",
    mon_stop: "Disconnect",
    mon_lost: "port lost, waiting for it to come back",
    mon_back: "reconnected",
    mon_fail: "could not open the UART log; press RESET on the board",
    mode_install: "install", mode_update: "update", mode_settings: "settings only",
    error: "Error",
    err_sn: "Serial number must be a decimal number 1..4294967295.",
    err_pin: "PIN / private key must be exactly 8 printable ASCII characters.",
    err_ssid: "WiFi SSID must be 1..32 bytes.",
    err_wifi_pass: "WiFi password must be 8..63 bytes.",
    err_too_long: "{key} is too long.",
  },
};

let lang = DEFAULT_LANG;
try {
  const saved = localStorage.getItem(STORE);
  if (saved && T[saved]) lang = saved;
} catch { /* storage unavailable */ }

export const getLang = () => lang;

/** Translate @p key, substituting {name} from @p vars. Unknown keys come back as is. */
export function t(key, vars = {}) {
  const s = T[lang][key] ?? T.en[key] ?? key;
  return s.replace(/\{(\w+)\}/g, (_, k) => (k in vars ? String(vars[k]) : `{${k}}`));
}

/** Apply the language to the document (static texts, <html lang>, <title>). */
export function apply(root = document) {
  document.documentElement.lang = lang;
  for (const el of root.querySelectorAll("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of root.querySelectorAll("[data-i18n-html]")) el.innerHTML = t(el.dataset.i18nHtml);
  for (const el of root.querySelectorAll("[data-i18n-placeholder]")) el.placeholder = t(el.dataset.i18nPlaceholder);
  document.title = t("title");
}

export function setLang(l) {
  if (!T[l]) return;
  lang = l;
  try {
    localStorage.setItem(STORE, l);
  } catch { /* storage unavailable */ }
  apply();
}
