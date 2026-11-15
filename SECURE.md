# Bezpieczny kanał CLU

Opcja `Secure CLU channel` → `CLU_SECURE_CHANNEL` (domyślnie wyłączona) dodaje obok protokołu
Grentona (UDP 1234) szyfrowany i uwierzytelniony kanał dla wywołań CLU↔CLU (`CLUxxx:execute/get/set`
z OM.LUA) oraz narzędzi na komputerze (`tools/clu_sec.py`).

## Po co

Protokół Grentona: AES-128-CBC, jeden klucz projektu na wszystkie CLU, stały IV, bez MAC i bez
numerów sekwencyjnych. Kto zna klucz projektu (np. z podsłuchu konfiguracji lub PIN), ten może
czytać, podrabiać i powtarzać polecenia. Bezpieczny kanał zapewnia:

- każde CLU ma własną parę kluczy X25519 (tworzoną przy pierwszym starcie, NVS `sec/sk`);
- klucze innych CLU i komputerów są **przypinane** (NVS `sec/pins`) — zaufanie nie zależy od
  klucza projektu;
- sesje z forward secrecy (Noise IK), wiadomości szyfrowane AES-256-GCM, odrzucanie powtórzeń (okno 64);
- koszt wywołania: wyłącznie AES-GCM; uzgadnianie kluczy (kilka operacji X25519) odbywa się raz na
  godzinę na parę CLU.

## Protokół

[Noise](https://noiseprotocol.org/noise.html) `Noise_IK_25519_AESGCM_SHA256` (rewizja 34),
prologue `opengr8on-sec-v1`, UDP `CLU_SEC_PORT` (domyślnie 1235), liczby w formacie little-endian:

| Typ | Ramka |
|---|---|
| PROBE `08` | `nonce(4)`, dopełnione zerami do 48 B (odpowiedź nie jest większa od zapytania) |
| PROBE_RESP `09` | `nonce(4) wersja(1) sn(4) klucz_publiczny(32)` |
| HS_INIT `01` | `idx_nadawcy(4)` + wiadomość 1 Noise (ładunek 14 B: `wersja(1)=2 rodzaj(1)='c'|'h' sn(4) znacznik_czasu_ms(8)`; `sn`=0 dla hosta) |
| HS_RESP `02` | `idx_nadawcy(4) idx_odbiorcy(4)` + wiadomość 2 Noise |
| DATA `04` | `idx_odbiorcy(4) licznik(8)` + AES-256-GCM(tekst) + tag(16); AAD = 13 B nagłówka, nonce = licznik |

Ochrona przed powtórzeniem HS_INIT: inicjator z zegarem wpisuje czas Unix (ms, ściśle rosnący);
odpowiadający odrzuca wiadomość 1 ze znacznikiem starszym niż ostatni przyjęty dla danego klucza
(ten sam znacznik tylko jako retransmisja w trakcie uzgodnienia) oraz różniącym się od własnego
zegara o więcej niż 1 h. Znacznik 0 (inicjator bez zegara) wyłącza tę ochronę.

W ramkach DATA przesyłany jest binarny protokół wywołań (liczby little-endian):

| Typ | Ładunek |
|---|---|
| REQ `01` | `id(4)` + skrypt Lua |
| RESP `02` | `id(4) status(1: 0=OK, 1=błąd)` + wartość |
| ACK `03` | `id(4)` |

Wywołujący powtarza REQ co 300 ms do ACK, potem co 2 s do RESP. Odpowiadający deduplikuje po
`(ip, id)` (4 wpisy, 30 s): duplikat dostaje ACK i zapamiętaną odpowiedź, skrypt wykonuje się raz.
Kanał przyjmuje tylko wywołania Lua (na pozostałe odpowiada błędem). Odpowiadający wysyła
dane w sesji dopiero po otrzymaniu pierwszej wiadomości DATA od inicjatora. Inicjator rozpoczyna
nowe uzgodnienie po godzinie lub po 2^20 wiadomościach; stara sesja działa jeszcze 2 min, a
nieużywana wygasa po 10 min. Odpowiadający obsługuje do 4 uzgodnień na sekundę. Komunikaty z
nieznanym kluczem pozostają bez odpowiedzi.

Klucz publiczny można też odczytać protokołem Grentona: `req:upgrade:v1:pubkey` →
`resp:upgrade:v1:pubkey:<sn>:<64 hex>:<port>` (wyłącznie informacyjnie — o zaufaniu decyduje
przypięcie, a nie ta odpowiedź).

## Parowanie

Nowy klucz jest przypinany **tylko w oknie konfiguracji** (`CLU_SETUP_WINDOW_S`: po starcie,
po naciśnięciu przycisku setup, w trybie awaryjnym). Przy `CLU_SETUP_WINDOW_S = 0` okno jest zawsze
otwarte, więc każdy klucz zostaje przypięty przy pierwszym kontakcie (TOFU) — do użytku
produkcyjnego ustaw ograniczony czas okna.

**CLU ↔ CLU — automatycznie.** Po starcie (`SYSTEM.Init`) CLU wysyła `checkAlive()` kanałem
bezpiecznym do każdego CLU zadeklarowanego w OM.LUA (po kolei), więc parowanie nie czeka na
pierwsze wywołanie. Pierwsze wywołanie z OM.LUA do CLU o nieznanym adresie:

1. PROBE na port 1235. Brak odpowiedzi → **CLU legacy**: wywołanie jest wysyłane przez UDP 1234,
   ponowna próba po 10 min.
2. Odpowiedź z kluczem: jeśli numer seryjny jest już przypięty z innym kluczem → odmowa
   (wywołanie zwraca `nil`, ostrzeżenie w logu). Jeśli okno konfiguracji tego CLU jest zamknięte →
   legacy.
3. Uzgodnienie (HS_INIT wysyłane drugi raz bez zmian, gdy odpowiedź się spóźnia). Drugie CLU
   akceptuje uzgodnienie tylko we własnym oknie, a klucz przypina dopiero po pierwszej poprawnej
   wiadomości DATA (dowód, że inicjator zakończył uzgodnienie), więc zgubiona odpowiedź nie
   pozostawia jednostronnego przypięcia. Odmowa → legacy. Sukces → klucz przypięty po obu stronach.

W praktyce: zrestartuj oba CLU (albo naciśnij przycisk setup na obu) i wykonaj dowolne wywołanie
między nimi w czasie trwania okna. **Przypiętego CLU nic nie cofa do trybu legacy** (ochrona przed
atakiem obniżenia wersji): gdy nie odpowiada w bezpiecznym kanale, wywołanie kończy się błędem.

**Komputer** (`pip install noiseprotocol cryptography`):

```sh
tools/clu_sec.py pubkey 192.168.1.20     # numer seryjny i odcisk klucza CLU
tools/clu_sec.py pair 192.168.1.20       # okno konfiguracji CLU musi być otwarte
tools/clu_sec.py exec 192.168.1.20 'CLU0:get(4)'
```

Klucz komputera: `~/.config/opengr8on/host.key`, przypięte CLU: `~/.config/opengr8on/clus.json`.

**Wymiana urządzenia / nowy klucz** (np. po wyczyszczeniu NVS): w oknie konfiguracji CLU wyślij
`req:upgrade:v1:forget:<sn albo ≥8 znaków odcisku>` protokołem Grentona (odpowiedź
`resp:upgrade:v1:forget:<liczba>`), a po stronie komputera wykonaj `tools/clu_sec.py forget <sn>`.

Odcisk klucza (pierwsze 8 B SHA-256 klucza publicznego) jest widoczny w logu przy starcie:
`sec: listening on udp/1235, key <odcisk>, <n> pinned peer(s)`.

## Wyłączenie protokołu Grentona

`CLU_SEC_DISABLE_LEGACY` (wymaga `CLU_SETUP_WINDOW_S` ≠ 0). Poza oknem konfiguracji port UDP 1234
odpowiada tylko na `req:upgrade:v1:pubkey` oraz `checkAlive()` (discovery i tak działa wyłącznie w
oknie). Wywołania Lua, `req_reset`, TFTP, diagnostyka i odświeżanie modułów otrzymują odpowiedź
`resp:ERROR`, a `clientReport` nie jest wysyłany (podgląd na żywo w OM nie działa). Wywołania do
innych CLU są wysyłane wyłącznie bezpiecznym kanałem — CLU legacy (w tym każde oryginalne CLU
Grentona) są nieosiągalne ze skryptów. W oknie konfiguracji wszystko działa jak zwykle, więc OM nadal
programuje i debuguje CLU.

## Model zagrożeń

- Chroni: poufność i integralność wywołań między CLU z włączoną opcją oraz z narzędzi na komputerze,
  odporność na powtórzenia, podszycie się bez klucza prywatnego oraz obniżenie wersji przypiętego CLU.
- Nie chroni: dopóki protokół Grentona działa (bez `CLU_SEC_DISABLE_LEGACY` albo w oknie
  konfiguracji), każdy, kto zna klucz projektu, może wykonywać kod Lua na CLU przez UDP 1234. Klucz
  prywatny jest zapisany w NVS bez szyfrowania: fizyczny dostęp do pamięci flash oznacza dostęp do
  klucza. Pierwsze parowanie ufa urządzeniu, które odpowie w czasie okna konfiguracji.
- Ładunek uzgodnienia (numer seryjny) nie zawiera znacznika czasu: powtórzona wiadomość 1 zajmuje
  slot sesji, ale nie daje dostępu (bez znajomości klucza efemerycznego inicjatora).

## Testy

- `test/host/test_secure.c` — zgodność Noise IK bajt w bajt z pakietem Pythona `noiseprotocol`
  (`fixtures/gen_noise_vectors.py`), okno powtórzeń, przypinanie, dwa silniki komunikujące się przez
  symulowaną sieć.
- `test/host/test_sec_tool.sh` (`make test-sec-tool`) — test `tools/clu_sec.py` z silnikiem C przez
  UDP.
- `test/host/test_udp_locked` — `CLU_SEC_DISABLE_LEGACY`.
- `test/emulator/udp/run_udp.py --secure` — dwa CLU w esp-emu: automatyczne parowanie, 3 wywołania
  w każdą stronę przez UDP 1235 bez ruchu CLU↔CLU na 1234, powtórzona wiadomość zignorowana, utracony
  pakiet = błąd po upływie limitu czasu. Ten sam obraz bez `--secure`: brak odpowiedzi na PROBE →
  legacy.
