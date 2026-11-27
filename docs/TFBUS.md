# Sterowniki modułów TF-Bus

Zachowanie sterowników modułów oświetleniowych. Ramki i działanie metod odpowiadają przechwytom
prawdziwej CLU sterującej tymi modułami (testy hosta `test_drivers` porównują je bajt po bajcie).
Zdarzenia (OnValueChange, OnSwitchOn, …) nie są widoczne na magistrali. Zdarzenia SMART_PANEL_FM_4
(przyciski, gesty, zbliżenie, włączenie / wyłączenie wyświetlacza, czujniki temperatury i światła)
sprawdzono na sprzęcie: ta sama akcja na panelu daje te same zdarzenia na prawdziwej CLU i na tym
firmware. Semantyka zdarzeń pozostałych modułów jest niezweryfikowana. Stan modułów: [README](../README.md#moduły-tf-bus).

- [Wspólne zasady](#wspólne-zasady)
- [DIMMER_MOSFET_DIN](#dimmer_mosfet_din)
- [LED_RGBW_DIN](#led_rgbw_din)

## Wspólne zasady

- Każde wywołanie metody wysyła ramki, także gdy wartość się nie zmienia.
- Argument `Ramp` pominięty = cecha RampTime; podany (także 0) = wysyłany bez zmian.
- Przy wykrywaniu i adresowaniu modułów prawdziwa CLU nic do nich nie wysyła. Ustawienia
  (RampTime, StartLevel, korekcje, DistributedLogicGroup, …) idą na magistralę z `set()` w
  `OnInit` OM.LUA. Firmware robi tak samo, a dodatkowo po ponownym zaadresowaniu modułu (powrót
  online) wysyła mu ostatni stan ustawiony ze skryptów.
- Moduł nieobecny (bez ACK na ADDR_ASSIGN albo 3 nieodebrane odpytania) nie jest odpytywany;
  dostaje jedną próbę ADDR_ASSIGN co ok. 40 ms przez 60 s od RESET-u wykrywania, potem co 3 s.
  Gdy nic nie odpowiada, SEEK jest wysyłany 4 razy.
- Moduły odpowiadają na DISCOVER dopiero po ok. 576 ms, więc DISCOVER czeka 1 s. Jak prawdziwe CLU,
  firmware zbiera wszystkie odpowiedzi z tej sekundy i adresuje każdy moduł (zrzut: panel po 369 ms,
  RELAY po 577 ms, oba dostały adres); moduły unikają kolizji tylko różnym opóźnieniem odpowiedzi.
  Przy starcie moduły z rejestru są adresowane po SN przed DISCOVER, który szuka tylko nowych.
- Odpytywanie: wszystkie moduły po kolei bez przerw, potem magistrala stoi
  `CLU_TFBUS_POLL_ROUND_GAP_MS` (domyślnie 12 ms) przed następną rundą. Prawdziwe CLU: runda co
  ok. 17 ms przy 2 modułach, 19,4 ms przy 3 (ok. 2,35 ms na moduł + ok. 12 ms przerwy). 0 = bez
  przerwy (dwa razy większe obciążenie magistrali).
- PowerSupplyVoltage (klasa 12) LED_RGBW_DIN, RELAY_DIN_4 i IO_MODULE_DIN_8: rekord statusu
  `02 02 10 [U LE u16, 0,01 V]`. Value% = U / MaxValue × 100 bez zaokrąglania (prawdziwe CLU:
  23,13 V przy MaxValue 24 → 96,375). Zdarzenia zmiany co Sensitivity, OnOutOfRange / OnInRange
  przy przekroczeniu MinValue / MaxValue. IO_MODULE_DIN_8 wysyła ten sam rekord, niesprawdzony.
- Czujniki paneli (temperatura, światło): pierwsza ramka stanu po zaadresowaniu przychodzi przed
  uruchomieniem OM.LUA. Wartość jest zapamiętana i trafia do Value w chwili utworzenia obiektu, bez
  zdarzeń (panel raportujący tylko zmiany miałby inaczej Value 0 do następnej zmiany).
- `Switch*` z `Time > 0` po upływie czasu przywraca poprzednią wartość.
- `Hold*` (wywoływane z OnHold przycisku): przerwa dłuższa niż ok. 0,5 s zaczyna nowe
  przytrzymanie w przeciwnym kierunku (od końca zakresu zawsze w stronę środka).

## DIMMER_MOSFET_DIN

hwType 0x1a, obiekt DIMM (klasa 5).

| Ramka (MODULE_CMD) | Kiedy |
|---|---|
| `06 05 00 [0, ms LE u16, 0, 0]` SET_RAMP_TIME | przed każdym poziomem; SetRampTime od razu |
| `0c 03 00 [0, poziom LE u16]` poziom 0–1000 | SetValue, Switch*, HoldValue; koniec czasowego Switch* bez SET_RAMP_TIME |
| `0e 03 00 [0, poziom LE u16]` | SetStartLevel |
| `0f 04 00 [0, 1, grupa LE u16]` | DistributedLogicGroup |

- Status: `02 | 01 02 10 [poziom LE u16] | 00 01 01 [wejścia]` (poziom = zadany).
- Min/MaxValue obcinają wartość w CLU (OnOutOfRange); SwitchOn → MaxValue albo (Mode 1) ostatnia
  wartość powyżej MinValue, SwitchOff → MinValue.
- HoldValue(Ramp): każde wywołanie to krok 100/Ramp pełnego zakresu 0–1 (Ramp 3000 → 0,033) i
  SET_RAMP_TIME = Ramp; na końcu zakresu wartość stoi.
- Ostatnia wartość (Mode LastValue) zapisywana w NVS, przetrwa restart CLU.

## LED_RGBW_DIN

hwType 0x18: LEDRGB (klasa 9), LED_CHANNEL ×4 (klasa 49), LED_CCT ×2 (klasa 72),
PowerSupplyVoltage (klasa 12). Kanały modułu: 0 R, 1 G, 2 B, 3 W.

| Ramka (MODULE_CMD) | Kiedy |
|---|---|
| `06 05 00 [kanał, t LE u16, 0, 0]` SET_RAMP_TIME, t w 10 ms | przed wartością kanału |
| `05 02 10 [kanał, wartość]` 0–255 | wartość kanału |
| `07 03 10 [kanał, % LE u16]` | Red/Green/BlueCorrection (koryguje moduł) |
| `13 03 00 [kanał, kierunek 1 w górę / 2 w dół / 0 stop, cel]` | Hold* (rampę prowadzi moduł) |
| `0f 04 00 [kanał, 1, grupa LE u16]` | DistributedLogicGroup (LED_CHANNEL) |

Status: `01 | 02 02 10 [U LE u16, 0,01 V]` (PowerSupplyVoltage) oraz po zmianie
`n | 04 02 08 [kanał, wartość] …` — wartości kanałów, z których korzystają LED_CHANNEL i LED_CCT.

### LEDRGB

- SET_RAMP_TIME dla wszystkich czterech kanałów (dwie ramki: 3, 2 i 1, 0), potem jedna ramka z
  czterema wartościami. SetRampTime wysyła rampy od razu, Min/MaxValue nic.
- Model koloru w CLU: HSV + R/G/B + W. Hue w pełnych stopniach, Saturation/Value z dwoma
  miejscami po przecinku (obcięte), R/G/B = obcięte składowe × 255 przeliczone z tych wartości.
  Czerń i szarość: Saturation 0, Hue bez zmian.
- SwitchOn → MaxValue (W bez zmian), SwitchOff → wszystkie kanały z W na 0, Switch według
  „cokolwiek świeci”.
- Hold*: jeden kierunek wspólny dla wszystkich Hold*. HoldValue: krok obcięte 100/Ramp;
  HoldHue: krok 36000/Ramp stopni, zawija 0 ↔ 359, rampa 0; `Hold<kanał>Value`: krok
  zaokrąglone 25500/Ramp.

### LED_CHANNEL

- Jedna ramka na wywołanie: rampa i wartość tylko tego kanału. Rampa w pełnych sekundach
  (zaokrąglona: 200 ms → 0, 999 ms → 1 s, 1500 ms → 2 s).
- SetValue obcięte do [MinValue, MaxValue] i do liczby całkowitej; SwitchOn → MaxValue albo
  (Mode 1) ostatnia wartość > 0; SwitchOff → 0. RampTime i Min/MaxValue nie są wysyłane.
- Hold*: pierwsze wywołanie startuje rampę modułu w stronę MaxValue / MinValue (t = Ramp),
  kolejne ją podtrzymują, stop ok. 0,55 s po ostatnim.
- Value pochodzi z raportu modułu (jak w prawdziwej CLU `get()` jest opóźnione); zdarzenia za nim.

### LED_CCT

- LED_CCT 1 steruje kanałami 0 (ciepły) i 1 (zimny), LED_CCT 2 kanałami 2 i 3.
- p = ColorTemp% / 100 = (6500 − ColorTemp) / 3800; ciepły = zaokrąglone Value × p,
  zimny = Value − ciepły. Domyślnie 4600 K (50%).
- Każdy kanał dostaje własną rampę, tak by oba skończyły razem:
  t = Ramp/10 · Δ · 255 / Δkanału, gdzie Δ to zmiana Value/255 albo p, a Δkanału liczona od
  wartości zgłoszonej ostatnio przez moduł.
- Hold Value*: oba kanały w stronę podziału MaxValue (w górę) albo 0; HoldColorTemp*: ciepły do
  Value i zimny do 0 (cieplej) albo odwrotnie. Stop ok. 0,25 s po ostatnim wywołaniu, potem
  Value i ColorTemp wynikają z raportu modułu.
- Założenia niesprawdzone: rampa w pełnych sekundach jak w LED_CHANNEL, SwitchOff → 0.

---

# TF-Bus module drivers

Behavior of lighting module drivers. Frames and method behavior match captures of a real CLU
controlling these modules (host tests `test_drivers` compare them byte-for-byte). Events
(OnValueChange, OnSwitchOn, …) are not visible on the bus. SMART_PANEL_FM_4 events (buttons,
gestures, proximity, display on / off, temperature and light sensors) were checked on hardware:
the same panel action gives the same events on a real CLU and on this firmware. Event semantics of
the other modules are unverified. Module support status: [README](../README.md#moduły-tf-bus).

- [Common rules](#common-rules)
- [DIMMER_MOSFET_DIN](#dimmer_mosfet_din-1)
- [LED_RGBW_DIN](#led_rgbw_din-1)

## Common rules

- Every method call sends frames, even when the value does not change.
- `Ramp` argument omitted = RampTime property; provided (including 0) = sent as-is.
- During module discovery and addressing, a real CLU sends nothing to the modules. Settings
  (RampTime, StartLevel, corrections, DistributedLogicGroup, …) go onto the bus from `set()` in
  OM.LUA's `OnInit`. The firmware does the same, and additionally sends the last script-set state
  after re-addressing a module (coming back online).
- An absent module (ADDR_ASSIGN not ACKed, or 3 missed polls) is not polled; it gets one
  ADDR_ASSIGN try about every 40 ms for 60 s after the discovery RESET, then every 3 s. With nothing
  answering, SEEK is sent 4 times.
- Modules answer DISCOVER only after about 576 ms, so DISCOVER waits 1 s. Like the real CLU, the
  firmware collects every reply of that second and addresses each module (capture: a panel at
  369 ms, a RELAY at 577 ms, both assigned); modules avoid collisions only by answering at different
  times. At boot the registry modules are addressed by SN before DISCOVER, which then only looks for
  new ones.
- Polling: all modules back to back, then the bus stays idle for `CLU_TFBUS_POLL_ROUND_GAP_MS`
  (default 12 ms) before the next round. Real CLU: a round every ~17 ms with 2 modules, 19.4 ms with
  3 (about 2.35 ms per module + about 12 ms idle). 0 = no idle time (about twice the bus load).
- PowerSupplyVoltage (class 12) of LED_RGBW_DIN, RELAY_DIN_4 and IO_MODULE_DIN_8: status record
  `02 02 10 [U LE u16, 0.01 V]`. Value% = U / MaxValue × 100, not rounded (real CLU: 23.13 V with
  MaxValue 24 → 96.375). Change events every Sensitivity, OnOutOfRange / OnInRange when crossing
  MinValue / MaxValue. IO_MODULE_DIN_8 sends the same record, unverified.
- Panel sensors (temperature, light): the first state frame after addressing arrives before
  OM.LUA runs. The value is kept and becomes Value when the object is created, without events (a
  panel that reports only changes would otherwise show Value 0 until the next change).
- `Switch*` with `Time > 0` restores the previous value once the timer expires.
- `Hold*` (called from a button's OnHold): a gap longer than approx. 0.5 s starts a new hold in the
  opposite direction (from either end of the range, always toward the middle).

## DIMMER_MOSFET_DIN

hwType 0x1a, DIMM object (class 5).

| Frame (MODULE_CMD) | When |
|---|---|
| `06 05 00 [0, ms LE u16, 0, 0]` SET_RAMP_TIME | before every level; SetRampTime immediately |
| `0c 03 00 [0, level LE u16]` level 0–1000 | SetValue, Switch*, HoldValue; end of timed Switch* without SET_RAMP_TIME |
| `0e 03 00 [0, level LE u16]` | SetStartLevel |
| `0f 04 00 [0, 1, group LE u16]` | DistributedLogicGroup |

- Status: `02 | 01 02 10 [level LE u16] | 00 01 01 [inputs]` (level = target).
- Min/MaxValue clamp the value in the CLU (OnOutOfRange); SwitchOn → MaxValue or (Mode 1) last
  value above MinValue, SwitchOff → MinValue.
- HoldValue(Ramp): each call steps by 100/Ramp of the full 0–1 range (Ramp 3000 → 0.033) with
  SET_RAMP_TIME = Ramp; stops at the end of the range.
- Last value (Mode LastValue) is saved in NVS and survives CLU reboot.

## LED_RGBW_DIN

hwType 0x18: LEDRGB (class 9), LED_CHANNEL ×4 (class 49), LED_CCT ×2 (class 72),
PowerSupplyVoltage (class 12). Module channels: 0 R, 1 G, 2 B, 3 W.

| Frame (MODULE_CMD) | When |
|---|---|
| `06 05 00 [channel, t LE u16, 0, 0]` SET_RAMP_TIME, t in 10 ms | before channel value |
| `05 02 10 [channel, value]` 0–255 | channel value |
| `07 03 10 [channel, % LE u16]` | Red/Green/BlueCorrection (applied by module) |
| `13 03 00 [channel, dir 1 up / 2 down / 0 stop, target]` | Hold* (ramp driven by module) |
| `0f 04 00 [channel, 1, group LE u16]` | DistributedLogicGroup (LED_CHANNEL) |

Status: `01 | 02 02 10 [U LE u16, 0.01 V]` (PowerSupplyVoltage) and after a change
`n | 04 02 08 [channel, value] …` — channel values used by LED_CHANNEL and LED_CCT.

### LEDRGB

- SET_RAMP_TIME for all four channels (two frames: 3, 2 and 1, 0), followed by one frame with all
  four values. SetRampTime sends ramps immediately, Min/MaxValue send nothing.
- Color model in CLU: HSV + R/G/B + W. Hue in whole degrees, Saturation/Value truncated to two
  decimal places, R/G/B = truncated components × 255 computed from those values. Black and gray:
  Saturation 0, Hue unchanged.
- SwitchOn → MaxValue (W unchanged), SwitchOff → all channels including W to 0, Switch based on
  "anything lit".
- Hold*: single direction shared across all Hold*. HoldValue: step truncated 100/Ramp;
  HoldHue: step 36000/Ramp degrees, wraps 0 ↔ 359, ramp 0; `Hold<channel>Value`: step rounded
  25500/Ramp.

### LED_CHANNEL

- One frame per call: ramp and value for that channel only. Ramp in whole seconds (rounded:
  200 ms → 0, 999 ms → 1 s, 1500 ms → 2 s).
- SetValue clamped to [MinValue, MaxValue] and truncated to integer; SwitchOn → MaxValue or
  (Mode 1) last value > 0; SwitchOff → 0. RampTime and Min/MaxValue are not sent.
- Hold*: first call starts the module's ramp toward MaxValue / MinValue (t = Ramp), subsequent calls
  keep it alive, stops approx. 0.55 s after the last call.
- Value comes from the module report (as on a real CLU, `get()` lags behind); events follow it.

### LED_CCT

- LED_CCT 1 controls channels 0 (warm) and 1 (cool), LED_CCT 2 controls channels 2 and 3.
- p = ColorTemp% / 100 = (6500 − ColorTemp) / 3800; warm = rounded Value × p,
  cool = Value − warm. Default 4600 K (50%).
- Each channel gets its own ramp so both finish together:
  t = Ramp/10 · Δ · 255 / Δchannel, where Δ is the change in Value/255 or p, and Δchannel is
  computed from the value last reported by the module.
- Hold Value*: both channels toward the MaxValue split (up) or 0; HoldColorTemp*: warm to Value and
  cool to 0 (warmer) or vice versa. Stops approx. 0.25 s after the last call, after which Value and
  ColorTemp come from the module report.
- Unverified assumptions: ramp in whole seconds as in LED_CHANNEL, SwitchOff → 0.
