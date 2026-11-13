# Sterowniki modułów TF-Bus

Zachowanie sterowników modułów oświetleniowych. Ramki i działanie metod odpowiadają przechwytom
prawdziwej CLU sterującej tymi modułami (testy hosta `test_drivers` porównują je bajt po bajcie).
Zdarzenia (OnValueChange, OnSwitchOn, …) nie są widoczne na magistrali, więc ich semantyka jest
niezweryfikowana. Stan modułów: [README](README.md#moduły-tf-bus).

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
