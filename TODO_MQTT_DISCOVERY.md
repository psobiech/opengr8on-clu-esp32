# MQTT discovery: device-based configs with abbreviated keys — TODO

Status legend: `[~]` done but unverified on hardware, `[ ]` open.

## Why

Today every entity has its own retained config, `<prefix>/<component>/opengr8on_CLU<sn>/<id>/config`.
Each one repeats the full `device` and `origin` blocks and the long key names. A project with a few hundred
objects ends up with a few hundred retained topics, and most of each payload is those repeated blocks.

Home Assistant 2024.11+ also accepts **device-based discovery**: one retained config per device,
`<prefix>/device/<object_id>/config`, with every entity listed in `cmps`. Together with abbreviated keys
(`stat_t`, `cmd_t`, `uniq_id`, `dev`, `avty`, ...) this cuts both the number of topics and the bytes sent.

There are no users yet, so this replaces the per-entity configs outright: no migration flow, no per-entity
fallback mode. It needs Home Assistant 2024.11 or newer.

## Target shape

One config per HA device: the CLU, and each module (`via_device` = the CLU).

```json
{
  "dev": {"ids": ["opengr8on_CLU<sn>"], "name": "...", "mf": "OpenGr8on", "mdl": "...", "hw": "...", "sw": "...", "sn": "..."},
  "o": {"name": "opengr8on-clu-esp32", "sw": "<fw>"},
  "avty_t": "<root>/status",
  "cmps": {
    "DOU0001": {"p": "switch", "uniq_id": "opengr8on_CLU<sn>_DOU0001", "name": "...", "~": "<root>/DOU0001",
                "stat_t": "~/state", "cmd_t": "~/set"},
    "...": {}
  }
}
```

- `object_id`: `opengr8on_CLU<sn>` for the CLU, `opengr8on_CLU<sn>_<module>` for a module (= the device `ids`).
- Component keys stay the current `entity.id`, and `uniq_id` keeps its current format.
- Availability: the CLU status goes at device level. A TF-Bus module's device config gets
  `avty` [CLU status, module link] + `avty_mode: all` at device level, because all of that device's
  components share it, except the link entity itself, which overrides it per component with the CLU status only.
- Abbreviations: use only keys from HA's `abbreviations.py` (`DEVICE_ABBREVIATIONS` for `dev`/`o`). Add a lookup
  table in mqtt_core.lua that `config_payload` applies when it encodes, so the kinds keep their readable keys.
  Do not abbreviate inside JSON-schema light payloads: they are state/command payloads, not config.

## Steps

- [ ] **Payload size.** Measure the largest device config (a CLU with many virtual objects and user features,
  a SMART_PANEL with 16 buttons + pages) against `OUTBOX_MAX` (8 KiB), the esp-mqtt buffer
  (`CONFIG_MQTT_BUFFER_SIZE`) and the broker's max message size. If the CLU device can get too big, split its
  components over several device configs that share the same `dev.ids`. HA allows this: each config is its own
  discovery "node" with a distinct `object_id`, e.g. `..._system`, `..._var`, `..._objects_<n>`.
- [ ] **Abbreviation table** + host test: every key emitted by every kind maps to a documented abbreviation
  or is passed through unchanged.
- [ ] **Builder.** Group `entities` by device. Keep a `cmps` entry per entity (`p` = `entity.comp`).
  `publish_discovery` publishes one config per device (or per chunk). Hidden module-link entities still have
  no component.
- [ ] **Stale configs.** Keep the persisted `mqtt.configs` topic list, now holding device topics. A device that
  disappears is cleared as today (empty retained payload). Per-entity topics left on dev brokers are not in the
  new list, so the first publish clears them the same way. No extra code is needed.
- [ ] **Removing a component from a device that stays.** Publish the device config with that component reduced
  to `{"p": "<platform>"}` once (HA deletes the entity), then drop it from `cmps` on the next build. Persist the
  component ids per device next to `mqtt.configs`, so removals are noticed across reboots.
- [ ] **README:** discovery topics, Home Assistant 2024.11+ requirement.
- [ ] **Tests** (test_mqtt.lua): device config shape and abbreviations; one config per device; module
  availability at device level; component removal; chunking when a device exceeds the size limit;
  stale device topics cleared.
- [ ] Verify on a real HA.
