// Page texts. data-i18n="key" sets textContent, data-i18n-html="key" innerHTML (static, trusted
// strings below), data-i18n-placeholder="key" the placeholder. Polish is the default.

export const LANGS = { pl: "Polski", en: "English" };
export const DEFAULT_LANG = "pl";
const STORE = "opengr8on-flasher-lang";

const RELEASES = "https://github.com/psobiech/opengr8on-clu-esp32/releases";

const T = {
  pl: {
    title: "Flasher opengr8on CLU",
    intro: "Instalacja i aktualizacja firmware ESP32 CLU oraz zapis jego ustawień przez USB w Chrome lub Edge na komputerze, a aktualizacja także przez sieć. Ustawienia (także hasła) nie są nigdzie wysyłane; przy aktualizacji przez sieć hasło OTA idzie tylko do CLU.",
    no_serial: "Ta przeglądarka nie obsługuje Web Serial. Użyj Chrome lub Edge na komputerze albo wgraj firmware z linii poleceń (zob. README). Aktualizacja przez sieć działa.",
    s1_title: "Firmware",
    s1_text: `Pobierz obraz dla swojej płytki ze <a href="${RELEASES}" target="_blank" rel="noopener">strony wydań</a> (<code>opengr8on-clu-{płytka}-{wersja}.bin</code>; do aktualizacji przez sieć wystarczy mniejszy <code>…-ota.bin</code>) i wybierz go tutaj. Niepotrzebny przy „Tylko ustawienia”.`,
    th_board: "Płytka", th_file: "Plik", th_net: "Sieć", ph_ver: "{wersja}",
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
    t_opt: "opcjonalne", t_rec: "zalecane", t_req_wifi: "wymagane na płytkach z WiFi",
    sn_hint: "Puste: losowy przy pierwszym starcie (widać go w logu UART i przy wyszukiwaniu w Object Managerze).",
    pin_hint: "Ustaw własny i zapisz go: Object Manager pyta o PIN przy dodawaniu CLU. Puste: losowy, do odczytania tylko z logu UART zaraz po wgraniu.",
    l_sn: "Numer seryjny", l_pin: "PIN / klucz prywatny", ph_pin: "8 znaków",
    lg_wifi: "WiFi", wifi_only: "(tylko płytki z WiFi)",
    l_ssid: "SSID", l_pass: "Hasło",
    lg_mqtt: "MQTT / Home Assistant",
    l_uri: "Adres brokera", l_user: "Użytkownik",
    mqtt_off: "MQTT wyłączone (nawet jeśli wbudowane)",
    lg_ota: "Aktualizacje przez sieć (OTA)",
    ota_hint: "Puste = wyłączone. Z hasłem: <code>curl -u ota:HASŁO --data-binary @opengr8on-clu-{płytka}-{wersja}-ota.bin http://IP_CLU:8080/ota</code>. Zwykły HTTP: hasło idzie przez sieć jawnym tekstem.",
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
    err_ota_pass: "Hasło OTA musi mieć 8..64 bajty.",
    err_too_long: "Pole {key} jest za długie.",
    m_ota: "<b>Przez sieć (OTA)</b> — wysyła firmware do działającej CLU, bez USB; zachowuje ustawienia, stan i dysk a:\\",
    lg_ota_net: "CLU w sieci",
    l_clu_addr: "Adres CLU (IP lub nazwa, opcjonalnie :port; domyślnie 8080)",
    l_ota_pass: "Hasło OTA",
    ota_net_hint: "CLU musi już mieć hasło OTA (ustawione w kroku 2 przy wgrywaniu przez USB albo <code>mknvs.py --ota-pass</code>). Wybierz obraz dla tej samej płytki — CLU sprawdza układ, ale nie płytkę. Przeglądarka może zapytać o dostęp do urządzeń w sieci lokalnej: zezwól. Gdy połączenie zostanie zablokowane, pojawi się link do strony aktualizacji na samej CLU.",
    ota_btn: "Wyślij przez sieć",
    app_only: "sam obraz aplikacji: tylko aktualizacja przez sieć",
    err_app_only: "To sam obraz aplikacji (…-ota.bin): przez USB wgraj pełny obraz ze strony wydań (bez -ota). Ten nadaje się tylko do aktualizacji przez sieć.",
    err_no_app: "W tym obrazie nie ma aplikacji do wysłania.",
    err_clu_addr: "Podaj adres CLU, np. 192.168.1.50 albo 192.168.1.50:8080.",
    err_ota_auth: "Złe hasło OTA.",
    err_ota_unexpected: "CLU odpowiedziała {status}: czy to właściwy adres i firmware z aktualizacją przez sieć?",
    err_ota_lost: "Połączenie przerwane w trakcie wysyłania (obraz za duży, zły plik albo CLU się zrestartowała).",
    err_ota_status: "CLU odrzuciła obraz ({status}): {text}",
    log_ota_check: "Łączenie z {base}, sprawdzanie hasła…",
    log_ota_blocked: "Brak połączenia z CLU z tej strony.",
    log_ota_upload: "Hasło poprawne. Wysyłanie {size} MB (to może potrwać minutę)…",
    cfg_ota_hint: "Aktualizacja przez sieć nie zmienia ustawień: CLU zachowuje swoje. Żeby je zmienić, wgraj przez USB.",
    ota_mode_off: "Niedostępne przy zapisie nowych ustawień (krok 2) — ustawienia zapisuje się tylko przez USB.",
    done_ota: "Gotowe: CLU zapisała firmware i uruchamia się ponownie. Za ~20 s nowa wersja będzie widoczna na {base}/",
    ota_blocked: "Przeglądarka nie pozwoliła tej stronie (HTTPS) połączyć się z CLU (HTTP w sieci lokalnej) — działa to tylko w Chrome po zgodzie na dostęp do sieci lokalnej — albo CLU nie odpowiada. Wgraj firmware na stronie samej CLU: zapisz obraz aplikacji, otwórz stronę, wybierz plik i wpisz hasło.",
    ota_open: "Otwórz stronę aktualizacji CLU",
    ota_save_app: "Zapisz obraz aplikacji",
  },
  en: {
    title: "opengr8on CLU flasher",
    intro: "Install or update the ESP32 CLU firmware and write its settings over USB from Chrome or Edge on a desktop computer, and update it over the network too. The settings (passwords included) are never sent anywhere; a network update sends the OTA password to the CLU only.",
    no_serial: "This browser has no Web Serial. Use Chrome or Edge on a desktop computer, or flash from the command line (see the README). The network update works.",
    s1_title: "Firmware",
    s1_text: `Download the image for your board from the <a href="${RELEASES}" target="_blank" rel="noopener">releases page</a> (<code>opengr8on-clu-{board}-{version}.bin</code>; for a network update the smaller <code>…-ota.bin</code> is enough) and select it here. Not needed for "Settings only".`,
    th_board: "Board", th_file: "File", th_net: "Network", ph_ver: "{version}",
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
    t_opt: "optional", t_rec: "recommended", t_req_wifi: "required on WiFi boards",
    sn_hint: "Empty: random on first boot (shown in the UART log and in the Object Manager search).",
    pin_hint: "Set your own and note it down: Object Manager asks for the PIN when adding the CLU. Empty: random, readable only from the UART log right after flashing.",
    l_sn: "Serial number", l_pin: "PIN / private key", ph_pin: "8 characters",
    lg_wifi: "WiFi", wifi_only: "(WiFi boards only)",
    l_ssid: "SSID", l_pass: "Password",
    lg_mqtt: "MQTT / Home Assistant",
    l_uri: "Broker URI", l_user: "Username",
    mqtt_off: "MQTT off (even if built in)",
    lg_ota: "Network updates (OTA)",
    ota_hint: "Empty = off. With a password: <code>curl -u ota:PASSWORD --data-binary @opengr8on-clu-{board}-{version}-ota.bin http://CLU_IP:8080/ota</code>. Plain HTTP: the password crosses the network in clear text.",
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
    err_ota_pass: "OTA password must be 8..64 bytes.",
    err_too_long: "{key} is too long.",
    m_ota: "<b>Over the network (OTA)</b> — send the firmware to a running CLU, no USB; keeps the settings, state and the a:\\ drive",
    lg_ota_net: "CLU on the network",
    l_clu_addr: "CLU address (IP or host name, optional :port; default 8080)",
    l_ota_pass: "OTA password",
    ota_net_hint: "The CLU must already have an OTA password (set in step 2 when flashing over USB, or <code>mknvs.py --ota-pass</code>). Pick the image for the same board: the CLU checks the chip, not the board. The browser may ask for access to devices on the local network: allow it. If the connection is blocked, a link to the CLU's own update page appears.",
    ota_btn: "Send over the network",
    app_only: "application image only: network update only",
    err_app_only: "This is the application image only (…-ota.bin): over USB, flash the full image from the releases page (without -ota). This one is for network updates only.",
    err_no_app: "No application to send in this image.",
    err_clu_addr: "Enter the CLU address, e.g. 192.168.1.50 or 192.168.1.50:8080.",
    err_ota_auth: "Wrong OTA password.",
    err_ota_unexpected: "The CLU answered {status}: right address, and firmware with network updates?",
    err_ota_lost: "Connection lost during the upload (image too large, wrong file, or the CLU restarted).",
    err_ota_status: "The CLU rejected the image ({status}): {text}",
    log_ota_check: "Connecting to {base}, checking the password…",
    log_ota_blocked: "This page cannot reach the CLU.",
    log_ota_upload: "Password OK. Sending {size} MB (may take a minute)…",
    cfg_ota_hint: "A network update does not change the settings: the CLU keeps its own. To change them, flash over USB.",
    ota_mode_off: "Not available while writing new settings (step 2): settings are written over USB only.",
    done_ota: "Done: the CLU has written the firmware and restarts. In ~20 s the new version shows at {base}/",
    ota_blocked: "The browser did not let this page (HTTPS) reach the CLU (HTTP on the local network) — only Chrome allows it, after you grant local network access — or the CLU does not answer. Update on the CLU's own page: save the application image, open the page, pick the file and enter the password.",
    ota_open: "Open the CLU update page",
    ota_save_app: "Save the application image",
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
