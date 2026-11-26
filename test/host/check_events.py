#!/usr/bin/env python3
"""Event coverage of the supported OM interfaces.

Every event that an interface file of a supported module / CLU / virtual object declares must be
listed below: as fired (the firmware posts it under that index) or as Missing (with the reason).
The check fails when the interface files and this table disagree: an undeclared index, a wrong
name, a declared event that is not listed, or a pattern that matches no file.

Usage: python3 check_events.py [--report] <device-interfaces dir>
  --report  also list the Missing events (the known gaps)
"""
import argparse
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import NamedTuple


class Missing(NamedTuple):
    """Declared event the firmware never posts."""
    name: str
    reason: str


class Remapped(NamedTuple):
    """Declared event that OM registers (and the firmware fires) under another index."""
    name: str
    fired_index: int
    reason: str


def fired(*names: str) -> dict:
    """Events 0..n-1, all fired, in index order."""
    return dict(enumerate(names))


def missing_all(names_by_index: dict, reason: str) -> dict:
    """All events of an object type not posted, for the same reason."""
    return {index: Missing(name, reason) for index, name in names_by_index.items()}


PSU_NOT_DECODED = "no supply voltage record known in this module's status (needs a capture)"
PSU_EVENTS = {0: "OnValueChange", 1: "OnValueLower", 2: "OnValueRise", 3: "OnOutOfRange", 6: "OnInRange"}
SENSOR_EVENTS = fired("OnValueChange", "OnValueRise", "OnValueLower", "OnOutOfRange")
BUTTON_EVENTS = fired("OnValueChange", "OnSwitchOn", "OnSwitchOff", "OnShortPress", "OnLongPress", "OnHold",
                      "OnClick")
LEVEL_EVENTS = fired("OnValueChange", "OnSwitchOn", "OnSwitchOff", "OnValueRise", "OnValueLower", "OnOutOfRange")
ROLLER_FIRED = {0: "OnStateChange", 1: "OnUp", 2: "OnDown", 3: "OnStart", 4: "OnStop", 8: "OnPositionChange"}
LAMEL_NOT_DECODED = "lamel state / position not decoded from the module status"
ROLLER_LAMEL_MISSING = {
    6: Missing("OnLamelClosed", LAMEL_NOT_DECODED),
    7: Missing("OnLamelOpen", LAMEL_NOT_DECODED),
    9: Missing("OnLamelPositionChange", LAMEL_NOT_DECODED),
}

# Interface file pattern -> object class -> event index -> name | Missing | Remapped.
# Class 0 is the CLU / GATE itself. Names match any version of the interface (old files use
# OnChange / OnRise / OnLower instead of OnValueChange / OnValueRise / OnValueLower).
EVENT_SUPPORT = {
    # --- local CLU / GATE (clu_state.h)
    "clu_ZWAVE_2_ft00000003_fv203_ht00000013_hv00000001.xml": {
        0: {
            0: "OnInit",
            **missing_all({8: "OnBusVoltageChange", 9: "OnBusVoltageLower", 10: "OnBusVoltageRise",
                           11: "OnBusVoltageOutOfRange", 12: "OnBusVoltageInRange"},
                          "no bus voltage measurement"),
            13: "OnTimeChange",
        },
    },
    "clu_GATE_HTTP_ft00000003_fv00000578_ht00000012_hv00000002.xml": {
        0: {1: Remapped("OnInit", 0, "OM generates add_event(0, OnInit) for the GATE too")},
    },
    # --- TF-Bus modules (tfbus_drivers)
    "module_2_0_IO_MODULE_DIN_8_fv*.xml": {
        3: BUTTON_EVENTS,
        4: fired("OnValueChange", "OnSwitchOn", "OnSwitchOff"),
        12: PSU_EVENTS,
    },
    "module_2_0_RELAY_DIN_4_fv*.xml": {
        4: {**fired("OnValueChange", "OnSwitchOn", "OnSwitchOff"),
            3: Missing("OnOverload", "overload flag not decoded from the module status")},
        12: PSU_EVENTS,
    },
    "module_2_0_ROLLER_SHUTTER_DIN_fv*.xml": {
        24: {**ROLLER_FIRED, **ROLLER_LAMEL_MISSING,
             5: Missing("OnOvercurrent", "overcurrent flag not decoded from the module status")},
        12: missing_all(PSU_EVENTS, PSU_NOT_DECODED),
    },
    "module_2_0_ROLLER_SHUTTER_DIN_3_fv*.xml": {
        24: {**ROLLER_FIRED, **ROLLER_LAMEL_MISSING},
        12: missing_all(PSU_EVENTS, PSU_NOT_DECODED),
    },
    "module_2_0_TOUCH_PANEL_FM_4_fv*.xml": {
        7: BUTTON_EVENTS,
        12: missing_all(PSU_EVENTS, PSU_NOT_DECODED),
        20: SENSOR_EVENTS,
        21: SENSOR_EVENTS,
    },
    "module_SMART_PANEL_FM_4_fv*.xml": {
        19: missing_all({0: "OnPageOpen", 1: "OnPageClose", 2: "OnDraw", 4: "OnTherm1ModeButtonClick",
                         5: "OnTherm2ModeButtonClick", 6: "OnTherm3ModeButtonClick",
                         7: "OnTherm4ModeButtonClick"}, "never sent by a real panel on the bench: page commands, texts and swipes report no page flag"),
        20: SENSOR_EVENTS,
        21: SENSOR_EVENTS,
        29: {**fired("OnGestureUp", "OnGestureDown", "OnGestureLeft", "OnGestureRight", "OnProximityDetect"),
             5: Missing("OnPageChange", "never sent by a real panel on the bench (SetNextPage / SetPageNr, swipes)"),
             6: "OnDisplayOn", 7: "OnDisplayOff"},
        30: BUTTON_EVENTS,
    },
    "module_2_0_DIMMER_MOSFET_DIN_fv*.xml": {
        5: LEVEL_EVENTS,
    },
    "module_2_0_LED_RGBW_DIN_fv*.xml": {
        9: LEVEL_EVENTS,
        12: PSU_EVENTS,
        49: LEVEL_EVENTS,
        72: fired("OnValueChange", "OnColorTempChange", "OnSwitchOn", "OnSwitchOff", "OnOutOfRange"),
    },
    "module_2_0_ANALOG_DIN_fv*.xml": {
        12: fired("OnValueChange", "OnValueLower", "OnValueRise", "OnOutOfRange", "OnSwitchOn", "OnSwitchOff",
                  "OnInRange"),
        13: missing_all(fired("OnValueChange", "OnValueLower", "OnValueRise", "OnOutOfRange", "OnSwitchOn",
                              "OnSwitchOff"), "AnalogOUT not implemented"),
    },
    "module_one_wire_fv*.xml": {
        23: {**SENSOR_EVENTS, 4: "OnConnect", 5: "OnDisconnect"},
    },
    # --- virtual objects (clu_lua/objects.lua, clu_http/http.lua)
    "object_timer_v*.xml": {6: fired("OnTimer", "OnStart", "OnStop", "OnPause")},
    "object_calendar_v1.xml": {15: fired("OnCalendar", "OnStart", "OnStop", "OnCancel")},
    "object_scheduler_v1.xml": {16: fired("OnHarmonogram", "OnStart", "OnStop")},
    "object_thermostat_v2.xml": {18: fired("OnChange", "OnStart", "OnStop", "OnOutOn", "OnOutOff", "OnHolidayModeOn",
                                           "OnHolidayModeOff")},
    "object_push_v1.xml": {31: fired("OnSend", "OnOverflow")},
    "object_sunrise_sunset_calendar_v3.xml": {39: fired("OnStart", "OnStop", "OnSunrise", "OnSunset",
                                                        "OnSunriseSunsetChange", "OnDay", "OnNight")},
    "object_presence_sensor_v2.xml": {40: fired("OnStart", "OnStop", "OnSwitchOn", "OnSwitchOff")},
    "object_event_scheduler_v1.xml": {43: fired("OnStart", "OnStop", "OnEvent", "OnRuleAdd", "OnRuleDelete")},
    "object_statistics_v1.xml": {47: fired("OnValueChange")},
    "object_statistics_pulse_v1.xml": {48: {1: "OnPulseTriggered"}},
    "object_http_listener_v*.xml": {120: fired("OnRequest")},
    "object_http_request_v*.xml": {121: fired("OnRequestSent", "OnResponse")},
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("interfaces_dir", type=Path)
    parser.add_argument("--report", action="store_true", help="list the Missing events")
    arguments = parser.parse_args()

    problems = []
    for pattern, support_by_class in EVENT_SUPPORT.items():
        interface_files = sorted(arguments.interfaces_dir.glob(pattern))
        if not interface_files:
            problems.append(f"{pattern}: no interface file matches")
            continue
        declared_by_class = declared_events(interface_files)
        problems += compare(pattern, declared_by_class, support_by_class)

    if arguments.report:
        print_gaps()
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"\n{'FAILED' if problems else 'PASSED'} ({len(problems)} problems)")
    return 1 if problems else 0


def declared_events(interface_files: list) -> dict:
    """Object class -> event index -> set of names, over all versions of one interface."""
    declared_by_class = {}
    for interface_file in interface_files:
        root = ElementTree.parse(interface_file).getroot()
        if root.tag == "CLU":
            interface_objects = [(0, root.find("interface"))]
        else:
            interface_objects = [(int(element.get("class")), element) for element in root.iter("object")
                                 if element.get("class") is not None]
        for object_class, element in interface_objects:
            if element is None:
                continue
            for event in element.iter("event"):
                names_by_index = declared_by_class.setdefault(object_class, {})
                names_by_index.setdefault(int(event.get("address")), set()).add(event.get("name"))
    return declared_by_class


def compare(pattern: str, declared_by_class: dict, support_by_class: dict) -> list:
    """Differences between the declared events and the support table of one interface pattern."""
    problems = []
    for object_class in sorted(declared_by_class.keys() | support_by_class.keys()):
        declared = declared_by_class.get(object_class, {})
        supported = support_by_class.get(object_class)
        if supported is None:
            problems.append(f"{pattern} class {object_class}: not listed (declares {format_events(declared)})")
            continue
        for index in sorted(declared.keys() | supported.keys()):
            where = f"{pattern} class {object_class} event {index}"
            if index not in supported:
                problems.append(f"{where}: {'/'.join(sorted(declared[index]))} not listed")
            elif index not in declared:
                problems.append(f"{where}: listed but not declared")
            elif entry_name(supported[index]) not in declared[index]:
                problems.append(f"{where}: listed as {entry_name(supported[index])}, "
                                f"declared as {'/'.join(sorted(declared[index]))}")
    return problems


def entry_name(entry) -> str:
    return entry if isinstance(entry, str) else entry.name


def format_events(names_by_index: dict) -> str:
    return ", ".join(f"{index}={'/'.join(sorted(names))}" for index, names in sorted(names_by_index.items())) or "none"


def print_gaps():
    print("Missing / remapped events:")
    for pattern, support_by_class in EVENT_SUPPORT.items():
        for object_class, supported in support_by_class.items():
            for index, entry in sorted(supported.items()):
                if isinstance(entry, Missing):
                    print(f"  {pattern} class {object_class} event {index} {entry.name}: {entry.reason}")
                elif isinstance(entry, Remapped):
                    print(f"  {pattern} class {object_class} event {index} {entry.name} fired as "
                          f"{entry.fired_index}: {entry.reason}")
    print()


if __name__ == "__main__":
    sys.exit(main())
