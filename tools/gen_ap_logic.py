#!/usr/bin/env python3
"""Generate platform/port_ap_logic_data.inc: the Archipelago tracker's logic as C++ tables.

The rules come from lilDavid's PopTracker pack for the Metroid Prime AP world
(https://github.com/lilDavid/MetroidPrimeAP-PoptrackerPack, MIT). The pack is
not vendored; clone it and run::

    python3 tools/gen_ap_logic.py <pack dir> > platform/port_ap_logic_data.inc

Each location's access rules are compiled to token arrays that
platform/port_ap_logic.cpp evaluates the way PopTracker does. Three things are
resolved here instead of at run time:

- A section's rules are its room's rules with its own appended (PopTracker's
  parent merge).
- The pack has the player mark where a shuffled elevator leads and what
  shield a door has. The port knows the seed, so an elevator room's two rules
  become one Elevator token (the room is reached from whichever elevator
  leads to it), and $can_open and @doors/ become Door tokens that carry the
  disc's lock for the seed's door colours and blast shields to be applied to.
- Trick codes become an index into the trick table.
"""

import json
import os
import re
import sys

AREA_FILES = ["tallon.json", "chozo.json", "magmoor.json", "phen.json", "mines.json", "rules.json"]

NEVER = {"NoLogic", "False", "Softlocks", "ElevatorsRandom"}

# Functions that return an accessibility level; the rest return a boolean.
LEVEL_FUNCS = {"can_xray", "can_thermal", "can_combat_omega_pirate", "can_crashed_frigate",
               "can_crashed_frigate_front"}

# The pack's door colours under the apworld's lock names.
DOOR_LOCK = {"IceBeam": "Ice Beam", "WaveBeam": "Wave Beam", "PlasmaBeam": "Plasma Beam",
             "PowerBeam": "Power Beam Only"}

# Rules the pack lacks but the apworld has, appended to a room's own rules.
EXTRA_RULES = {
    # Transport Tunnel E leads onto Great Tree Hall's upper level, which opens
    # straight into Hydro Access Tunnel (apworld: Transport Tunnel E door 1,
    # sub_region_door_index=0 with an always-true override). Without it a seed
    # that reaches Tallon only from the Mines East elevator, without Wave Beam
    # and Thermal, showed the rest of Tallon as a sequence break.
    ("Tallon Overworld", "Hydro Access Tunnel"): [
        "@Tallon Overworld/Transport Tunnel E,$can_open|Tallon Overworld|Transport Tunnel E|Great Tree Hall",
    ],
    # Beating Flaahgra opens Sunchamber's door to Sun Tower Access (apworld:
    # Sunchamber door 0, rule can_flaahgra); the pack has only a sequence
    # break there. Without it a Sunchamber Lobby start, which can't go back
    # through Arboretum, reached nothing past Flaahgra.
    ("Chozo Ruins", "Sun Tower Access"): [
        "@Chozo Ruins/Sunchamber,@rules/can_flaahgra",
    ],
}

OPTIONAL, BRACE, LEVEL = 1, 2, 4


def load_json(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"^\s*//.*$", "", text, flags=re.M)
    text = re.sub(r",(\s*[\]}])", r"\1", text)
    return json.loads(text)


def load_doors(pack):
    """door_data.lua -> ({area: {door}}, {area: {door: (forward, reverse)}})"""
    text = open(os.path.join(pack, "scripts/logic/door_data.lua"), encoding="utf-8").read()
    missile_text, mix_text = text.split("MIX_IT_UP_DOORS")
    missile, mix = {}, {}
    for out, body in ((missile, missile_text), (mix, mix_text)):
        area = None
        for line in body.splitlines():
            m = re.match(r'\s*\["([^"|]+)"\] = \{\s*$', line)
            if m:
                area = out.setdefault(m.group(1), {})
                continue
            m = re.match(r'\s*\["([^"]+\|[^"]+)"\] = (.*?),?\s*$', line)
            if not m:
                continue
            value = m.group(2)
            if value == "true":
                area[m.group(1)] = True
            else:
                m2 = re.match(r'\{"(\w+)", (?:"(\w+)"|nil)\}', value)
                assert m2, line
                area[m.group(1)] = (m2.group(1), m2.group(2))
    return missile, mix


def load_tricks(pack):
    text = open(os.path.join(pack, "scripts/autotracking/ap/trick_mapping.lua"), encoding="utf-8").read()
    levels = {"nil": 0, "TrickDifficulty.EASY": 1, "TrickDifficulty.MEDIUM": 2, "TrickDifficulty.HARD": 3}
    tricks, by_code = [], {}
    for m in re.finditer(r'TrickItem\("([^"]+)", ([\w.]+), \{([^}]*)\}\)', text):
        for code in re.findall(r'"([^"]+)"', m.group(3)):
            by_code[code] = len(tricks)
        tricks.append((m.group(1), levels[m.group(2)]))
    return tricks, by_code


def load_location_ids(pack):
    text = open(os.path.join(pack, "scripts/autotracking/ap/location_mapping.lua"), encoding="utf-8").read()
    return {int(m.group(1)): m.group(2) for m in re.finditer(r'\[(\d+)\] = \{"@([^"]*)"\}', text)}


def port_location_ids():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "platform", "port_ap_locations.inc")
    return [int(m.group(1)) for m in re.finditer(r"^\{(\d+),", open(path).read(), flags=re.M)]


class Compiler:
    def __init__(self, pack):
        self.missile, self.mix = load_doors(pack)
        self.tricks, self.trick_codes = load_tricks(pack)
        self.nodes = []  # (area, room, section, [rule strings])
        self.doors, self.door_index = [], {}  # (area, source, destination, lock, missile)
        self.starts = []
        self.index = {}
        for name in AREA_FILES:
            for area in load_json(os.path.join(pack, "locations", name)):
                for room in area.get("children", []):
                    self.add(area["name"], room)

    def add(self, area, room):
        rules = room.get("access_rules", []) + EXTRA_RULES.get((area, room["name"]), [])
        self.index[f"{area}/{room['name']}"] = len(self.nodes)
        self.nodes.append((area, room["name"], None, [r.split(",") for r in rules]))
        for section in room.get("sections", []):
            own = section.get("access_rules", [])
            # PopTracker's parent merge: every parent rule with every own rule appended.
            if not own:
                merged = [r.split(",") for r in rules]
            elif not rules:
                merged = [r.split(",") for r in own]
            else:
                merged = [p.split(",") + o.split(",") for p in rules for o in own]
            self.index[f"{area}/{room['name']}/{section.get('name', '')}"] = len(self.nodes)
            self.nodes.append((area, room["name"], section.get("name", ""), merged))

    def door(self, area, source, destination, lock, missile):
        entry = (area, source, destination, lock, missile)
        if entry not in self.door_index:
            self.door_index[entry] = len(self.doors)
            self.doors.append(entry)
        return ("Door", str(self.door_index[entry]), 0, "Code::None")

    def can_open(self, area, source, destination):
        """The door of the pack's can_open(): None when the pack doesn't know it, else a Door token."""
        missile, mix = self.missile.get(area), self.mix.get(area)
        if missile is None or mix is None:
            return None
        is_missile = f"{source}|{destination}" in missile or f"{destination}|{source}" in missile
        colour = None
        if f"{source}|{destination}" in mix:
            colour = mix[f"{source}|{destination}"][0]
        elif f"{destination}|{source}" in mix:
            forward, reverse = mix[f"{destination}|{source}"]
            colour = reverse or forward
        if colour is None and not is_missile:
            return None
        return self.door(area, source, destination, DOOR_LOCK.get(colour), is_missile)

    def find(self, key):
        """PopTracker matches a location by the end of its path."""
        if key in self.index:
            return self.index[key]
        found = [i for k, i in self.index.items() if k.endswith("/" + key)]
        return found[0] if len(found) == 1 else None

    def token(self, text, where, inspect):
        """One rule token -> (kind, flags, id, arg, arg2) strings, 'true' or 'false'.

        The flags are returned with a constant result too, as an open brace
        marks the rest of its rule as inspect-only.
        """
        flags = 0
        s = text.strip()
        if s.startswith("{"):
            flags |= BRACE
            s = s[1:]
        if s.endswith("}"):
            s = s[:-1]
        if len(s) > 1 and s.startswith("[") and s.endswith("]"):
            flags |= OPTIONAL
            s = s[1:-1]
        if s == "":
            # Skipped inside an inspect-only rule; elsewhere a bare "[]" counts nothing (a sequence break).
            return ("true" if inspect or flags & BRACE else "false"), flags
        if s.startswith("^"):
            flags |= LEVEL
            s = s[1:]
            assert s.startswith("$"), where
        count = 1
        if not flags & LEVEL and not s.startswith("@") and ":" in s:
            s, n = s.split(":")
            count = int(n)

        if s in NEVER:
            return "false", flags
        if s.startswith("StartingRoom"):
            if s not in self.starts:
                self.starts.append(s)
            return ("Start", flags, str(self.starts.index(s)), 0, "Code::None"), flags
        if s.startswith("@doors/"):
            _, area, colour = s.split("/")
            kind, ident, arg, code = self.door(area, None, None, DOOR_LOCK[colour], False)
            return (kind, flags, ident, arg, code), flags
        if s.startswith("@"):
            node = self.find(s[1:])
            if node is None:
                sys.exit(f"{where}: unknown location {s}")
            return ("Node", flags, str(node), 0, "Code::None"), flags
        if s.startswith("$"):
            name, *args = s[1:].split("|")
            if name == "can_open":
                door = self.can_open(*args)
                if door is None:
                    return "true", flags
                kind, ident, arg, code = door
                return (kind, flags, ident, arg, code), flags
            if name == "can_backwards_lower_mines":
                return ("Code", flags, "Code::BackwardsLowerMines", 1, "Code::None"), flags
            if name == "trick":
                return ("Trick", flags, str(self.trick_codes[args[0]]), int(args[1]), "Code::None"), flags
            if name == "has":
                return ("Code", flags, f"Code::{args[0]}", int(args[1]) if len(args) > 1 else 1, "Code::None"), flags
            if bool(flags & LEVEL) != (name in LEVEL_FUNCS):
                sys.exit(f"{where}: {s} mixes a level function and a count")
            arg, beam = 0, "Code::None"
            for a in args:
                if a.lstrip("-").isdigit():
                    arg = int(a)
                else:
                    beam = f"Code::{a}"
            return ("Func", flags, f"Func::{name}", arg, beam), flags
        if s in self.trick_codes:
            index = self.trick_codes[s]
            return ("Trick", flags, str(index), self.tricks[index][1], "Code::None"), flags
        if not re.fullmatch(r"[A-Za-z]\w*", s) or s[0].islower():
            sys.exit(f"{where}: unknown code {s}")
        return ("Code", flags, f"Code::{s}", count, "Code::None"), flags

    def rule(self, tokens, where):
        """A rule's tokens, or None when a folded constant makes it unreachable."""
        out, inspect = [], False
        if tokens[0] == "ElevatorsNormal":
            # "ElevatorsNormal,@the room this elevator comes from,$can_access_elevators"
            assert len(tokens) == 3 and tokens[1].startswith("@") and where in self.index, where
            partner = self.find(tokens[1][1:])
            assert partner is not None, where
            out.append(("Elevator", 0, str(self.index[where]), partner, "Code::None"))
            tokens = tokens[2:]
        for text in tokens:
            result, flags = self.token(text, where, inspect)
            inspect = inspect or bool(flags & BRACE)
            if result == "true":
                if flags & BRACE:
                    out.append(("Empty", BRACE, "0", 0, "Code::None"))
                continue
            if result == "false":
                if not flags & OPTIONAL:
                    return None
                out.append(("Never", flags, "0", 0, "Code::None"))
                continue
            out.append(result)
        return out

    def emit(self, location_ids):
        tokens, rules, nodes = [], [], []
        for area, room, section, node_rules in self.nodes:
            where = f"{area}/{room}" + (f"/{section}" if section is not None else "")
            first, always = len(rules), not node_rules
            for r in node_rules:
                compiled = self.rule(r, where)
                if compiled is None:
                    continue
                rules.append((len(tokens), len(compiled)))
                tokens.extend(compiled)
            if not always and len(rules) == first:
                print(f"note: {where} has no reachable rule", file=sys.stderr)
            # No rules at all means always reachable; mark it, as "every rule dropped" means never.
            nodes.append((area, room, section, first, len(rules) - first, always))

        out = ["// Generated by tools/gen_ap_logic.py from the Metroid Prime AP PopTracker pack",
               "// (github.com/lilDavid/MetroidPrimeAP-PoptrackerPack, MIT, (c) 2024 lil David). Do not edit.",
               "", "const Token kTokens[] = {"]
        for kind, flags, ident, arg, arg2 in tokens:
            out.append(f"    {{Kind::{kind}, {flags}, static_cast<uint16_t>({ident}), {arg}, {arg2}}},")
        out += ["};", "", "// { first token, token count }", "const Rule kRules[] = {"]
        out += [f"    {{{first}, {count}}}," for first, count in rules]
        out += ["};", "", "// { area, room, section, first rule, rule count, always reachable }", "const Node kNodes[] = {"]
        for area, room, section, first, count, always in nodes:
            sec = "nullptr" if section is None else json.dumps(section)
            out.append(f"    {{{json.dumps(area)}, {json.dumps(room)}, {sec}, {first}, {count}, {str(always).lower()}}},")
        out += ["};", "", "// { name in the AP world's trick lists, difficulty (1 easy .. 3 hard, 0 = always) }",
                "const Trick kTricks[] = {"]
        out += [f"    {{{json.dumps(name)}, {level}}}," for name, level in self.tricks]
        out += ["};", "", "// { area, the two rooms (null for any door of the lock), lock on the disc, missile shield }",
                "const Door kDoors[] = {"]
        text = lambda v: "nullptr" if v is None else json.dumps(v)
        for area, source, destination, lock, missile in self.doors:
            out.append(f"    {{{text(area)}, {text(source)}, {text(destination)}, {text(lock)}, {str(missile).lower()}}},")
        out += ["};", "", "// starting_room_name without its spaces", "const char* const kStartRooms[] = {"]
        out += [f"    {json.dumps(name[len('StartingRoom'):])}," for name in self.starts]
        out += ["};", "", "// { AP location id, node }", "const LocationNode kLocationNodes[] = {"]
        for ident in port_location_ids():
            if ident not in location_ids or location_ids[ident] not in self.index:
                sys.exit(f"location {ident} has no section in the pack")
            out.append(f"    {{{ident}, {self.index[location_ids[ident]]}}},")
        out.append("};")
        return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pack = sys.argv[1]
    sys.stdout.write(Compiler(pack).emit(load_location_ids(pack)))


if __name__ == "__main__":
    main()
