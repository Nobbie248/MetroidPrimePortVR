#!/usr/bin/env python3
"""Writes platform/port_ap_world_data.inc: the rooms and elevators an
Archipelago seed's layout options name, with the ids the game knows them by.

Sources (randomprime, MIT, see NOTICE):
  src/pickup_meta.rs.in  ROOM_INFO: every room's name and MREA, per pak
  src/elevators.rs       the elevators: world, room and WorldTransporter id

  src/pickup_meta.rs.in  door_locations: every dock's door, door forces and
                         shield actors

and, when a MetroidAPrime checkout is given (its door table, MIT, see NOTICE):
  src/Enum.py, src/data/<area>.py  each room's doors: destination, lock and
                                   blast shield, in the order the apworld
                                   walks them

    python3 tools/gen_ap_world.py <randomprime checkout> <MetroidAPrime checkout> [output]

The MetroidAPrime apworld names an elevator by its room ("Transport to Chozo
Ruins West"), and puts the area in front of the four names two areas share
("Chozo Ruins: Transport to Tallon Overworld South").
"""
import ast
import os
import re
import sys

# Pak -> (the apworld's area name, MLVL), in the order the apworld searches its
# areas for a starting room's name.
PAKS = [
    ("metroid4.pak", "Tallon Overworld", 0x39F2DE28),
    ("metroid2.pak", "Chozo Ruins", 0x83F6FF6F),
    ("metroid6.pak", "Magmoor Caverns", 0x3EF8237C),
    ("metroid3.pak", "Phendrana Drifts", 0xA8BE6291),
    ("metroid5.pak", "Phazon Mines", 0xB1AC4D65),
    ("metroid7.pak", "Impact Crater", 0xC13B09D1),
    ("metroid1.pak", "Frigate Orpheon", 0x158EFE17),
]


def rooms(path):
    text = open(path).read()
    out = {}
    paks = list(re.finditer(r'\("([Mm]etroid\d\.pak)", &\[', text))
    for i, pak in enumerate(paks):
        end = paks[i + 1].start() if i + 1 < len(paks) else len(text)
        body = text[pak.end():end]
        out[pak.group(1).lower()] = [
            (m.group(2), int(m.group(1), 16), int(m.group(3), 16))
            for m in re.finditer(
                r'room_id: ResId::<res_id::MREA>::new\(0x([0-9A-Fa-f]+)\),\s*name: "([^"]*)",'
                r'\s*name_id: [^\n]*\n\s*mapa_id: ResId::<res_id::MAPA>::new\(0x([0-9A-Fa-f]+)\)', body)
        ]
    return out


def elevators(path):
    text = open(path).read()
    text = text[text.index("decl_elevators! {"):text.index("macro_rules! decl_spawn_rooms")]
    out = []
    for m in re.finditer(
            r'pak_name: "([^"]+)",\s*name: "([^"]*)",(?:[ \t]*// "([^"\n]+)"?[^\n]*)?\s*mlvl: 0x([0-9a-fA-F]+),'
            r'\s*mrea: 0x([0-9a-fA-F]+),\s*mrea_idx: \d+,\s*scly_id: 0x([0-9a-fA-F]+),'
            r'\s*room_id: 0x[0-9a-fA-F]+,\s*room_strg: 0x([0-9a-fA-F]+),'
            r'\s*hologram_strg: 0x([0-9a-fA-F]+),\s*control_strg: 0x([0-9a-fA-F]+)', text):
        if m.group(3) is None:
            continue  # the Crater's and the frigate's: not in any mapping
        # randomprime's own name for the room, its two lines split by a NUL.
        shown = m.group(2).replace("\\0", "\\n")
        out.append((m.group(1).lower(), m.group(3), int(m.group(4), 16), int(m.group(5), 16),
                    int(m.group(6), 16), shown, [int(m.group(i), 16) for i in (7, 8, 9)]))
    return out


# The apworld's data file per area, in PAKS order.
AREA_FILES = ["TallonOverworld", "ChozoRuins", "MagmoorCaverns", "PhendranaDrifts", "PhazonMines"]

# The names slot_data uses; a door's lock or shield is an index into these.
LOCKS = ["Blue", "Wave Beam", "Ice Beam", "Plasma Beam", "Missile", "Power Beam Only", "Bomb",
         "None"]
SHIELDS = ["Bomb", "Charge Beam", "Flamethrower", "Ice Spreader", "Wavebuster", "Power Bomb",
           "Super Missile", "Missile", "Disabled", "None"]

# randomprime's patch_door: the docks whose door lies flat (MREA, dock).
VERTICAL = {
    (0x11BD63B7, 0), (0x0D72F1F7, 1), (0xFB54A0CB, 4), (0xE1981EFC, 0), (0x43E4CC25, 1),
    (0x37BBB33C, 1), (0xD8E905DD, 1), (0x21B4BFF6, 1), (0x3F375ECC, 2), (0xF517A1EA, 1),
    (0x8A97BB54, 1), (0xA20201D4, 0), (0xA20201D4, 1), (0x956F1552, 1), (0xC50AF17A, 2),
    (0x90709AAC, 1),
}


def door_objects(path):
    """(MREA, dock) -> (door id, rotation, door force ids, shield actor ids)."""
    text = open(path).read()
    out = {}
    marks = list(re.finditer(r"room_id: ResId::<res_id::MREA>::new\(0x([0-9A-Fa-f]+)\)", text))
    ids = lambda body: [int(i) for i in re.findall(r"instance_id: (\d+)", body)]
    for i, mark in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        for m in re.finditer(
                r"door_location: (None|Some\(.*?\)),\s*door_rotation: (None|Some\(\[(.*?)\]\)),"
                r"\s*door_force_locations: &\[(.*?)\],\s*door_shield_locations: &\[(.*?)\],"
                r"\s*dock_number: (\d+)", text[mark.end():end], re.S):
            if m.group(1) == "None":
                continue
            out[(int(mark.group(1), 16), int(m.group(6)))] = (
                ids(m.group(1))[0], [float(v) for v in m.group(3).split(",")],
                ids(m.group(4)), ids(m.group(5)))
    return out


def enums(path):
    out = {}
    for node in ast.parse(open(path).read()).body:
        if isinstance(node, ast.ClassDef):
            out[node.name] = {
                s.targets[0].id: s.value.value for s in node.body
                if isinstance(s, ast.Assign) and isinstance(s.value, ast.Constant)}
    return out


def apworld_doors(src, area_file, names):
    """[(room name, [(dock, {field: value})])] in the apworld's own order."""
    def value(node):
        if isinstance(node, ast.Attribute):
            return names[node.value.id][node.attr]
        return node.value

    out = []
    tree = ast.parse(open(os.path.join(src, "src", "data", area_file + ".py")).read())
    for node in ast.walk(tree):
        if not isinstance(node, ast.Dict):
            continue
        for key, call in zip(node.keys, node.values):
            if not (isinstance(call, ast.Call) and getattr(call.func, "id", "") == "RoomData"):
                continue
            doors = []
            for word in call.keywords:
                if word.arg != "doors":
                    continue
                for dock, door in zip(word.value.keys, word.value.values):
                    fields = {"default_destination": value(door.args[0])}
                    for field in door.keywords:
                        if field.arg in ("defaultLock", "blast_shield", "lock", "destination_area",
                                         "exclude_from_rando", "sub_region_door_index"):
                            fields[field.arg] = value(field.value)
                    doors.append((dock.value, fields))
            out.append((value(key), doors))
    return out


def door_lines(rando, apworld, by_pak):
    names = enums(os.path.join(apworld, "src", "Enum.py"))
    objects = door_objects(os.path.join(rando, "src", "pickup_meta.rs.in"))
    first = {}
    index = 0
    for pak, _, _ in PAKS:
        first[pak] = index
        index += len(by_pak[pak])
    lines = []
    for area, area_file in enumerate(AREA_FILES):
        pak = PAKS[area][0]
        # randomprime has "West Tower ".
        room_index = {name.strip(): first[pak] + i for i, (name, _, _) in enumerate(by_pak[pak])}
        for room, doors in apworld_doors(apworld, area_file, names):
            mrea = by_pak[pak][room_index[room] - first[pak]][1]
            for dock, door in doors:
                dest = door["default_destination"]
                flags = 0
                if door.get("exclude_from_rando"):
                    flags |= 1
                if (mrea, dock) in VERTICAL:
                    flags |= 2
                if door.get("destination_area") is not None:
                    flags |= 4
                door_id, rotation, forces, shields = objects.get((mrea, dock), (0, [0.0, 0.0, 0.0], [], []))
                if (mrea, dock) == (0xD5CDB809, 4):
                    # Main Plaza's one-way door: the port gives it a shield
                    # of its own (randomprime's ids).
                    forces, shields = [0x0002000F], [0x00020004]
                lock = lambda name: LOCKS.index(door[name]) if name in door else -1
                shield = SHIELDS.index(door["blast_shield"]) if "blast_shield" in door else -1
                lines.append(
                    "    {%d, %d, %d, %d, %d, %d, %d, %d, 0x%08X, {%sf, %sf, %sf}, {%s}, {%s}}," % (
                        room_index[room], dock, room_index.get(dest, -1) if dest else -1,
                        LOCKS.index(door.get("defaultLock", "Blue")), lock("lock"), shield, flags,
                        door.get("sub_region_door_index", -1), door_id,
                        rotation[0], rotation[1], rotation[2],
                        ", ".join("0x%08X" % i for i in (forces + [0, 0])[:2]),
                        ", ".join("0x%08X" % i for i in (shields + [0, 0])[:2])))
    head = ["", "const char* const kLocks[] = {%s};" % ", ".join('"%s"' % n for n in LOCKS),
            "const char* const kShields[] = {%s};" % ", ".join('"%s"' % n for n in SHIELDS), "",
            "// The apworld's doors, in the order it walks them: room (index in kRooms),",
            "// dock, the room behind it (-1 when it has none in this area's table), its",
            "// lock on the disc, the lock the table fixes (-1 none), its blast shield (-1",
            "// not said), flags (1 not recoloured, 2 lies flat, 4 leads to another area),",
            "// the door of the room behind it that this one also gives (-1 none), then",
            "// the Door object, its rotation, its damageable triggers and shield actors",
            "// (0 when the dock has no door object).", "const Door kDoors[] = {"]
    return head + lines + ["};"]


def main():
    src = sys.argv[1]
    apworld = sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    dest = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        here, "..", "platform", "port_ap_world_data.inc")
    by_pak = rooms(os.path.join(src, "src", "pickup_meta.rs.in"))
    area_of = {pak: area for pak, area, _ in PAKS}
    lines = ["// Generated by tools/gen_ap_world.py from randomprime's room, elevator and",
             "// door tables and the MetroidAPrime apworld's door table (both MIT, see",
             "// NOTICE). Do not edit.", "",
             "const char* const kAreas[] = {"]
    for _, area, _ in PAKS:
        lines.append('    "%s",' % area)
    lines += ["};", "", "// Area index, MLVL, MREA, MAPA, name.", "const Room kRooms[] = {"]
    count = 0
    for index, (pak, area, mlvl) in enumerate(PAKS):
        for name, mrea, mapa in by_pak[pak]:
            lines.append('    {%d, 0x%08X, 0x%08X, 0x%08X, "%s"},' % (index, mlvl, mrea, mapa, name))
            count += 1
    lines += ["};", "", "// The apworld's name, area index, MLVL, MREA, WorldTransporter id,",
              "// randomprime's name, and the STRGs of the room's scan, hologram and",
              "// control messages.",
              "const Elevator kElevators[] = {"]
    found = elevators(os.path.join(src, "src", "elevators.rs"))
    names = [entry[1] for entry in found]
    for pak, name, mlvl, mrea, scly, shown, strgs in found:
        if names.count(name) > 1:
            name = "%s: %s" % (area_of[pak], name)
        lines.append('    {"%s", %d, 0x%08X, 0x%08X, 0x%08X, "%s",\n     {0x%08X, 0x%08X, 0x%08X}},' % (
            name, [p for p, _, _ in PAKS].index(pak), mlvl, mrea, scly, shown, *strgs))
    lines.append("};")
    doors = door_lines(src, apworld, by_pak)
    lines += doors
    with open(dest, "w") as out:
        out.write("\n".join(lines) + "\n")
    print("%d rooms, %d elevators, %d doors -> %s" % (count, len(found), len(doors) - 13, os.path.normpath(dest)))


if __name__ == "__main__":
    main()
