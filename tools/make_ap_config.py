#!/usr/bin/env python3
"""Join an Archipelago Metroid Prime world to a MetroidPrimePort LOC dump.

Example::

    python3 tools/make_ap_config.py --locations Locations.py --dump port.log \
        --out archipelago.json --server ws://host:port --slot Player1

Add ``--spoiler seed.json --seed-out randomizer_seed.json`` to translate an
Archipelago spoiler into the port's native randomizer seed format. Repeat
``--dump`` to merge multiple port dumps; ``--world-map`` accepts additional or
overridden world-name to MLVL-asset-ID mappings.

Locations are paired inside each area by the *id delta* between the AP entity id
and the port's editor id, which is a per-area constant (measured on real data:
Chozo Ruins +1, the Tallon Overworld landing site +7). An AP location whose
counterpart is missing at that delta is reported as unmapped rather than paired
with the nearest key, because a wrong pairing reports another player's check.
Dump every area with every layer it can be in: a pickup that the game has not
built yet is not in the dump.

``--strict`` writes nothing unless every AP location has a key, which is the
check to run before playing a seed. ``--extra FILE.json`` supplies the ones the
join could not place, as ``{"<AP location name>": "<WORLD:AREA:ENTITY key>"}``,
and the report names the candidates.

The written config lists no items: the port then applies the AP world's own item
table (ids, progressive items, ammo rules), the same one a config without a
location table gets. Only the locations come from the join.
"""

import argparse
import ast
import contextlib
import io
import json
import os
import sys
import tempfile


try:
    from .rando_seed import read_dump, write_json
except ImportError:
    # Also supports loading this file directly by path, outside the tools package.
    _TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
    if _TOOLS_DIR not in sys.path:
        sys.path.insert(0, _TOOLS_DIR)
    from rando_seed import read_dump, write_json


# MLVL IDs read from the game's own world list.
WORLD_ASSET_IDS = {
    "Tallon Overworld": 0x39F2DE28,
    "Chozo Ruins": 0x83F6FF6F,
    "Magmoor Caverns": 0x3EF8237C,
    "Phendrana Drifts": 0xA8BE6291,
    "Phazon Mines": 0xB1AC4D65,
    "Impact Crater": 0xC13B09D1,
    "Space Pirate Frigate": 0x158EFE17,
}

# Spoiler item names the native randomizer seed can hold, with the port item,
# amount and capacity each grants. Names follow the AP world's Enum.py; the
# progressive and unlimited-ammo items have no single port item and become
# UnknownItem1 there.
# Each row is (port item name, amount, capacity, AP display name).
ITEM_DEFINITIONS = (
    ("PowerBeam", 1, 1, "Power Beam"),
    ("IceBeam", 1, 1, "Ice Beam"),
    ("WaveBeam", 1, 1, "Wave Beam"),
    ("PlasmaBeam", 1, 1, "Plasma Beam"),
    ("Missiles", 5, 5, "Missile Expansion"),
    ("ScanVisor", 1, 1, "Scan Visor"),
    ("MorphBallBombs", 1, 1, "Morph Ball Bomb"),
    ("PowerBombs", 1, 1, "Power Bomb Expansion"),
    ("Flamethrower", 1, 1, "Flamethrower"),
    ("ThermalVisor", 1, 1, "Thermal Visor"),
    ("ChargeBeam", 1, 1, "Charge Beam"),
    ("SuperMissile", 1, 1, "Super Missile"),
    ("GrappleBeam", 1, 1, "Grapple Beam"),
    ("XRayVisor", 1, 1, "X-Ray Visor"),
    ("IceSpreader", 1, 1, "Ice Spreader"),
    ("SpaceJumpBoots", 1, 1, "Space Jump Boots"),
    ("MorphBall", 1, 1, "Morph Ball"),
    ("CombatVisor", 1, 1, "Combat Visor"),
    ("BoostBall", 1, 1, "Boost Ball"),
    ("SpiderBall", 1, 1, "Spider Ball"),
    ("PowerSuit", 1, 1, "Power Suit"),
    ("GravitySuit", 1, 1, "Gravity Suit"),
    ("VariaSuit", 1, 1, "Varia Suit"),
    ("PhazonSuit", 1, 1, "Phazon Suit"),
    ("EnergyTanks", 1, 1, "Energy Tank"),
    ("UnknownItem1", 1, 1, "UnknownItem1"),
    ("HealthRefill", 10, 0, "HealthRefill"),
    ("UnknownItem2", 1, 1, "UnknownItem2"),
    ("Wavebuster", 1, 1, "Wavebuster"),
    ("Truth", 1, 1, "Artifact of Truth"),
    ("Strength", 1, 1, "Artifact of Strength"),
    ("Elder", 1, 1, "Artifact of Elder"),
    ("Wild", 1, 1, "Artifact of Wild"),
    ("Lifegiver", 1, 1, "Artifact of Lifegiver"),
    ("Warrior", 1, 1, "Artifact of Warrior"),
    ("Chozo", 1, 1, "Artifact of Chozo"),
    ("Nature", 1, 1, "Artifact of Nature"),
    ("Sun", 1, 1, "Artifact of Sun"),
    ("World", 1, 1, "Artifact of World"),
    ("Spirit", 1, 1, "Artifact of Spirit"),
    ("Newborn", 1, 1, "Artifact of Newborn"),
    ("Missiles", 5, 5, "Missile Launcher"),
    ("PowerBombs", 1, 1, "Power Bomb (Main)"),
    ("ChargeBeam", 1, 1, "Charge Beam (Power)"),
    ("ChargeBeam", 1, 1, "Charge Beam (Wave)"),
    ("ChargeBeam", 1, 1, "Charge Beam (Ice)"),
    ("ChargeBeam", 1, 1, "Charge Beam (Plasma)"),
)

ITEMS_BY_NAME = {row[3]: row[:3] for row in ITEM_DEFINITIONS}


class MakeAPConfigError(ValueError):
    """An input file cannot be safely joined to the port data."""


def _assigned_name(target):
    return target.id if isinstance(target, ast.Name) else None


def _module_assignments(tree):
    for statement in tree.body:
        if isinstance(statement, ast.Assign):
            for target in statement.targets:
                name = _assigned_name(target)
                if name:
                    yield name, statement.value
        elif isinstance(statement, ast.AnnAssign):
            name = _assigned_name(statement.target)
            if name and statement.value is not None:
                yield name, statement.value


def _integer_expression(node, context):
    if isinstance(node, ast.Constant) and isinstance(node.value, int) and not isinstance(
        node.value, bool
    ):
        return node.value
    if (
        isinstance(node, ast.UnaryOp)
        and isinstance(node.op, (ast.USub, ast.UAdd))
        and isinstance(node.operand, ast.Constant)
        and isinstance(node.operand.value, int)
        and not isinstance(node.operand.value, bool)
    ):
        return -node.operand.value if isinstance(node.op, ast.USub) else node.operand.value
    raise MakeAPConfigError("%s must be an integer literal" % context)


def _read_location_tables(tree):
    tables = {}
    table_order = []
    pickup_node = None
    every_location_node = None

    for name, value in _module_assignments(tree):
        if name.endswith("_location_table"):
            if not isinstance(value, ast.Dict):
                raise MakeAPConfigError("%s must be a dict literal" % name)
            table = {}
            for key_node, id_node in zip(value.keys, value.values):
                if key_node is None:
                    raise MakeAPConfigError("%s cannot contain dict unpacking" % name)
                try:
                    location_name = ast.literal_eval(key_node)
                except (ValueError, TypeError) as error:
                    raise MakeAPConfigError(
                        "%s has a non-literal location name" % name
                    ) from error
                if not isinstance(location_name, str):
                    raise MakeAPConfigError("%s location names must be strings" % name)
                table[location_name] = _integer_expression(
                    id_node, "%s location %r id" % (name, location_name)
                )
            tables[name] = table
            table_order.append(name)
        elif name == "PICKUP_LOCATIONS":
            pickup_node = value
        elif name == "every_location":
            every_location_node = value

    if not tables:
        raise MakeAPConfigError("Locations.py contains no *_location_table dicts")
    if not isinstance(pickup_node, ast.List):
        raise MakeAPConfigError("Locations.py must define PICKUP_LOCATIONS as a list literal")

    # Match the AP world's insertion-order merge, rather than assuming table
    # declaration order when an every_location dict unpack is available.
    merge_order = None
    if isinstance(every_location_node, ast.Dict):
        candidate = []
        for key_node, value_node in zip(every_location_node.keys, every_location_node.values):
            if (
                key_node is not None
                or not isinstance(value_node, ast.Name)
                or value_node.id not in tables
            ):
                candidate = []
                break
            candidate.append(value_node.id)
        if candidate:
            merge_order = candidate
    if merge_order is None:
        merge_order = table_order

    merged_locations = {}
    for table_name in merge_order:
        merged_locations.update(tables[table_name])

    pickups = []
    for index, entry in enumerate(pickup_node.elts):
        if not isinstance(entry, (ast.Tuple, ast.List)) or len(entry.elts) != 2:
            raise MakeAPConfigError(
                "PICKUP_LOCATIONS entry %d must be a 2-tuple" % index
            )
        level_node, entity_node = entry.elts
        if not isinstance(level_node, ast.Attribute):
            raise MakeAPConfigError(
                "PICKUP_LOCATIONS entry %d level must be an enum attribute" % index
            )
        level_name = level_node.attr
        entity_id = _integer_expression(
            entity_node, "PICKUP_LOCATIONS entry %d entity id" % index
        )
        pickups.append((level_name, entity_id))

    if len(merged_locations) != len(pickups):
        raise MakeAPConfigError(
            "Locations.py has %d merged locations but %d PICKUP_LOCATIONS entries; "
            "their insertion-order correspondence cannot be verified"
            % (len(merged_locations), len(pickups))
        )

    result = []
    for index, ((location_name, ap_location_id), (level_name, entity_id)) in enumerate(
        zip(merged_locations.items(), pickups)
    ):
        prefix = location_name.split(": ", 1)[0] if ": " in location_name else None
        if prefix and _normalize_world(prefix) != _normalize_world(level_name):
            raise MakeAPConfigError(
                "PICKUP_LOCATIONS entry %d is %s, but location %r has world prefix %r"
                % (index, level_name, location_name, prefix)
            )
        world_name = prefix if prefix else level_name.replace("_", " ")
        result.append(
            {
                "name": location_name,
                "ap_id": ap_location_id,
                "entity_id": entity_id,
                "area": (entity_id >> 16) & 0xFFFF,
                "world_name": world_name,
            }
        )
    return result


def _normalize_world(name):
    return "".join(character for character in name.casefold() if character not in " _")


def read_ap_locations(path):
    try:
        with open(path, "r", encoding="utf-8") as source:
            tree = ast.parse(source.read(), filename=path)
    except SyntaxError as error:
        raise MakeAPConfigError("cannot parse %s: %s" % (path, error)) from error
    return _read_location_tables(tree)


def read_extra(path):
    """Read manual gap fillers: {"<AP location name>": "<port key>"}.

    The join reports any AP location it could not place; this is how a user
    supplies the key for one (for example when the dump missed a pickup that is
    only built in an inactive layer)."""
    if path is None:
        return {}
    with open(path, "r", encoding="utf-8") as source:
        data = json.load(source)
    if not isinstance(data, dict):
        raise MakeAPConfigError("--extra must be a JSON object of AP location name -> port key")
    extras = {}
    for name, key in data.items():
        if not isinstance(name, str) or not isinstance(key, str):
            raise MakeAPConfigError("--extra entries must map a string name to a string key")
        try:
            world_hex, mrea_hex, editor_hex = key.split(":")
            int(world_hex, 16), int(mrea_hex, 16), int(editor_hex, 16)
        except ValueError as error:
            raise MakeAPConfigError(
                "--extra %r is not a WORLD:AREA:ENTITY key: %r" % (name, key)
            ) from error
        extras[name] = key.upper()
    return extras


def apply_extra(join, extras, ap_locations):
    """Fill gaps the join reported. Returns the number of entries applied."""
    known = {location["name"] for location in ap_locations}
    applied = 0
    for name, key in sorted(extras.items()):
        if name not in known:
            raise MakeAPConfigError("--extra names an unknown AP location: %r" % name)
        ap_id = join["unmapped"].get(name)
        if ap_id is None:
            if join["locations"].get(name) == key:
                continue
            raise MakeAPConfigError(
                "--extra %r was already mapped automatically; remove it or correct the dump" % name
            )
        join["locations"][name] = key
        join["ap_ids_by_port_key"][key] = ap_id
        del join["unmapped"][name]
        join["report"].append("manual: %r -> %s (--extra)" % (name, key))
        applied += 1
    return applied


def read_world_map(path):
    world_map = dict(WORLD_ASSET_IDS)
    if not path:
        return world_map
    with open(path, "r", encoding="utf-8") as source:
        additions = json.load(source)
    if not isinstance(additions, dict):
        raise MakeAPConfigError("world map must be a JSON object of names to asset IDs")
    for name, value in additions.items():
        if not isinstance(name, str):
            raise MakeAPConfigError("world map keys must be world-name strings")
        if isinstance(value, bool):
            raise MakeAPConfigError("world map value for %r is not an asset ID" % name)
        if isinstance(value, int):
            asset_id = value
        elif isinstance(value, str):
            try:
                asset_id = int(value, 0)
            except ValueError as error:
                raise MakeAPConfigError(
                    "world map value for %r must be an integer or 0x-prefixed ID" % name
                ) from error
        else:
            raise MakeAPConfigError("world map value for %r must be an integer" % name)
        if not 0 <= asset_id <= 0xFFFFFFFF:
            raise MakeAPConfigError("world map asset ID for %r is outside uint32" % name)
        world_map[name] = asset_id
    return world_map


def read_dumps(paths):
    locations = {}
    models = {}
    for path in paths:
        read_dump(path, locations, models, include_drops=False)
    return locations, models


def _format_area(area):
    return "0x%04X" % area


def _format_key(world_id, mrea_id, editor_id):
    return "%08X:%08X:%08X" % (world_id, mrea_id, editor_id)


def join_locations(ap_locations, dump_locations, world_map):
    """Pair AP locations and dump keys by rank within world/area groups."""
    ap_groups = {}
    unknown_groups = {}
    world_names = {}
    normalized_worlds = {}
    for name, asset_id in world_map.items():
        normalized_worlds.setdefault(_normalize_world(name), set()).add(asset_id)

    for location in ap_locations:
        world_name = location["world_name"]
        world_id = world_map.get(world_name)
        if world_id is None:
            matches = normalized_worlds.get(_normalize_world(world_name), set())
            world_id = next(iter(matches)) if len(matches) == 1 else None
        if world_id is None:
            unknown_groups.setdefault((world_name, location["area"]), []).append(location)
            continue
        group_key = (world_id, location["area"])
        ap_groups.setdefault(group_key, []).append(location)
        world_names.setdefault(world_id, world_name)

    reverse_worlds = {}
    for name, asset_id in sorted(world_map.items()):
        reverse_worlds.setdefault(asset_id, name)

    port_groups = {}
    for key, (item, amount, capacity, _hidden) in dump_locations.items():
        try:
            world_hex, mrea_hex, editor_hex = key.split(":")
            world_id, mrea_id, editor_id = (
                int(world_hex, 16), int(mrea_hex, 16), int(editor_hex, 16)
            )
        except (AttributeError, ValueError) as error:
            raise MakeAPConfigError("invalid location key returned by rando_seed: %r" % key) from error
        if capacity <= 0:
            continue
        port_groups.setdefault((world_id, (editor_id >> 16) & 0xFFFF), []).append(
            {
                "key": _format_key(world_id, mrea_id, editor_id),
                "editor_id": editor_id,
                "mrea_id": mrea_id,
                "item": item,
                "amount": amount,
                "capacity": capacity,
            }
        )

    joined = {}
    ap_ids_by_port_key = {}
    unmapped = {}
    report = []
    clean_areas = 0
    review_areas = 0
    missing_dump_areas = 0
    mismatch_areas = 0

    for world_name, area in sorted(unknown_groups, key=lambda pair: (pair[0].casefold(), pair[1])):
        entries = unknown_groups[(world_name, area)]
        report.append(
            "warning: unknown world %r area %s (%d AP locations); add it to --world-map"
            % (world_name, _format_area(area), len(entries))
        )

    all_groups = sorted(
        set(ap_groups) | set(port_groups),
        key=lambda group: (world_names.get(group[0], reverse_worlds.get(group[0], "")).casefold(), group[1], group[0]),
    )
    for world_id, area in all_groups:
        group_key = (world_id, area)
        ap_entries = sorted(
            ap_groups.get(group_key, []), key=lambda entry: (entry["entity_id"], entry["name"])
        )
        port_entries = sorted(
            port_groups.get(group_key, []), key=lambda entry: (entry["editor_id"], entry["key"])
        )
        world_name = world_names.get(world_id, reverse_worlds.get(world_id, "world 0x%08X" % world_id))
        label = "%s area %s" % (world_name, _format_area(area))
        if not ap_entries:
            report.append(
                "coverage gap: %s has %d dump location(s), but no AP locations"
                % (label, len(port_entries))
            )
            continue
        if not port_entries:
            missing_dump_areas += 1
            report.append(
                "coverage gap: %s has %d AP location(s), but no dump locations; dump this world/area"
                % (label, len(ap_entries))
            )
            for entry in ap_entries:
                report.append("  AP %r (id %d, entity %08X) -> no port key" % (
                    entry["name"], entry["ap_id"], entry["entity_id"] & 0xFFFFFFFF
                ))
                unmapped[entry["name"]] = entry["ap_id"]
            continue
        if len(ap_entries) != len(port_entries):
            mismatch_areas += 1
            report.append(
                "count mismatch: %s has %d AP location(s), %d dump location(s); "
                "no entries emitted for this area"
                % (label, len(ap_entries), len(port_entries))
            )
            for entry in ap_entries:
                report.append("  AP %r (id %d, entity %08X) -> unpaired" % (
                    entry["name"], entry["ap_id"], entry["entity_id"] & 0xFFFFFFFF
                ))
                unmapped[entry["name"]] = entry["ap_id"]
            for entry in port_entries:
                report.append("  dump %s -> unpaired" % entry["key"])
            continue

        # Pair by the dominant id delta rather than by rank. The AP entity id
        # and the port's editor id address the same object in two id spaces that
        # differ by a per-area constant (measured on real data: Chozo Ruins +1,
        # Tallon Overworld Landing Site +7). An entry with no counterpart at
        # that delta means the dump is missing it; guessing instead would report
        # another player's check, so emit nothing for it and say so.
        ranked = sorted(
            (
                (ap_entry["entity_id"] - port_entry["editor_id"], ap_entry, port_entry)
                for ap_entry, port_entry in zip(ap_entries, port_entries)
            ),
            key=lambda triple: (triple[0], triple[1]["entity_id"]),
        )
        counts = {}
        for delta, _, _ in ranked:
            counts[delta] = counts.get(delta, 0) + 1
        best = max(counts.values())
        modes = [delta for delta, count in counts.items() if count == best]

        pairs = []
        unmatched_ap = []
        if len(modes) == 1:
            mode = modes[0]
            port_by_editor = {entry["editor_id"]: entry for entry in port_entries}
            used = set()
            for ap_entry in ap_entries:
                port_entry = port_by_editor.get(ap_entry["entity_id"] - mode)
                if port_entry is None or port_entry["editor_id"] in used:
                    unmatched_ap.append(ap_entry)
                    continue
                used.add(port_entry["editor_id"])
                pairs.append((ap_entry, port_entry))
            unmatched_port = [entry for entry in port_entries if entry["editor_id"] not in used]
        else:
            # No dominant delta: fall back to rank pairing, which the caller
            # must review, and never emit silently.
            pairs = list(zip(ap_entries, port_entries))
            unmatched_port = []
            unmatched_ap = []

        for ap_entry, port_entry in pairs:
            delta = ap_entry["entity_id"] - port_entry["editor_id"]
            joined[ap_entry["name"]] = port_entry["key"]
            ap_ids_by_port_key[port_entry["key"]] = ap_entry["ap_id"]
            report.append(
                "  AP %r (id %d, entity %08X) -> %s (delta %+d)"
                % (
                    ap_entry["name"],
                    ap_entry["ap_id"],
                    ap_entry["entity_id"] & 0xFFFFFFFF,
                    port_entry["key"],
                    delta,
                )
            )
        for ap_entry in unmatched_ap:
            unmapped[ap_entry["name"]] = ap_entry["ap_id"]
            report.append(
                "  AP %r (id %d, entity %08X) -> no port counterpart at delta %+d; "
                "the dump is missing it (inactive layer or an undumped area)"
                % (
                    ap_entry["name"],
                    ap_entry["ap_id"],
                    ap_entry["entity_id"] & 0xFFFFFFFF,
                    modes[0] if len(modes) == 1 else 0,
                )
            )
        for port_entry in unmatched_port:
            report.append("  dump %s -> unpaired" % port_entry["key"])

        deltas = ", ".join("%+d" % delta for delta, _, _ in ranked)
        if len(modes) == 1 and not unmatched_ap and not unmatched_port:
            clean_areas += 1
            report.append(
                "clean: %s (%d AP / %d dump; delta %+d)" % (label, len(ap_entries), len(port_entries), modes[0])
            )
        else:
            review_areas += 1
            if len(modes) != 1:
                report.append(
                    "review: %s (%d AP / %d dump; deltas %s; no dominant delta, paired by rank)"
                    % (label, len(ap_entries), len(port_entries), deltas)
                )
            else:
                report.append(
                    "review: %s (%d AP / %d dump; delta %+d; %d unpaired AP, %d unpaired dump)"
                    % (
                        label,
                        len(ap_entries),
                        len(port_entries),
                        modes[0],
                        len(unmatched_ap),
                        len(unmatched_port),
                    )
                )

    mapped = len(joined)
    report.append(
        "summary: %d locations mapped, %d unmapped / %d areas clean / %d areas for review / "
        "%d areas missing from the dump (%d count mismatches; %d unknown-world areas)"
        % (mapped, len(unmapped), clean_areas, review_areas, missing_dump_areas, mismatch_areas,
           len(unknown_groups))
    )
    return {
        "locations": joined,
        "unmapped": unmapped,
        "ap_ids_by_port_key": ap_ids_by_port_key,
        "report": report,
        "clean_areas": clean_areas,
        "review_areas": review_areas,
        "missing_dump_areas": missing_dump_areas,
        "mismatch_areas": mismatch_areas,
        "unknown_world_areas": len(unknown_groups),
    }


def make_archipelago_config(join, server=None, slot=None):
    config = {
        "game": "Metroid Prime",
        "locations": {
            key: join["ap_ids_by_port_key"][key]
            for key in sorted(join["ap_ids_by_port_key"])
        },
    }
    if server is not None:
        config["server"] = server
    if slot is not None:
        config["slot"] = slot
    return config


def _same_player(player, slot_number):
    if isinstance(player, int) and not isinstance(player, bool):
        return player == slot_number
    if isinstance(player, str):
        try:
            return int(player) == slot_number
        except ValueError:
            return False
    return False


def make_randomizer_seed(spoiler, joined, models, slot_number=1, seed_name="ap"):
    if not isinstance(spoiler, dict) or not isinstance(spoiler.get("locations"), dict):
        raise MakeAPConfigError("spoiler must contain a locations object")
    seed_locations = {}
    skipped = 0
    for location_name, item_record in spoiler["locations"].items():
        port_key = joined.get(location_name)
        if port_key is None:
            skipped += 1
            continue
        if isinstance(item_record, str):
            item_name = item_record
            player = None
        elif isinstance(item_record, dict):
            item_name = item_record.get("item")
            player = item_record.get("player")
        else:
            item_name = None
            player = None
        item_data = ITEMS_BY_NAME.get(item_name) if isinstance(item_name, str) else None
        if (player is not None and not _same_player(player, slot_number)) or item_data is None:
            seed_locations[port_key] = {
                "item": "UnknownItem1",
                "amount": 0,
                "capacity": 0,
            }
        else:
            item, amount, capacity = item_data
            seed_locations[port_key] = {
                "item": item,
                "amount": amount,
                "capacity": capacity,
            }
    seed = {
        "seed": seed_name or "ap",
        "locations": {key: seed_locations[key] for key in sorted(seed_locations)},
        "models": {item: models[item] for item in sorted(models)},
    }
    return seed, skipped


def _load_spoiler(path):
    with open(path, "r", encoding="utf-8") as source:
        try:
            return json.load(source)
        except json.JSONDecodeError as error:
            raise MakeAPConfigError("cannot parse spoiler JSON %s: %s" % (path, error)) from error


def _emit_report(lines, stream=None):
    stream = stream or sys.stderr
    for line in lines:
        print(line, file=stream)


def _self_test():
    source = '''\
chozo_location_table = {
    "Chozo Ruins: A": 101,
    "Chozo Ruins: B": 102,
    "Chozo Ruins: C": 103,
    "Chozo Ruins: D": 104,
}
phen_location_table = {"Phendrana Drifts: E": 201}
tallon_location_table = {"Tallon Overworld: F": 301, "Tallon Overworld: G": 302}
mines_location_table = {"Phazon Mines: H": 401}
every_location = {**chozo_location_table, **phen_location_table,
                  **tallon_location_table, **mines_location_table}
PICKUP_LOCATIONS = [
    (MetroidPrimeLevel.Chozo_Ruins, 0x0002012D),
    (MetroidPrimeLevel.Chozo_Ruins, 0x0002012E),
    (MetroidPrimeLevel.Chozo_Ruins, 0x0002012F),
    (MetroidPrimeLevel.Chozo_Ruins, 0x00020133),
    (MetroidPrimeLevel.Phendrana_Drifts, 0x00050011),
    (MetroidPrimeLevel.Tallon_Overworld, 0x00040021),
    (MetroidPrimeLevel.Tallon_Overworld, 0x00040022),
    (MetroidPrimeLevel.Phazon_Mines, 0x00070031),
]
'''
    dump = """\
LOC 83F6FF6F:11111111:0002012C Missiles amount=5 capacity=5 model=2D7E6590 acs=A9B8E446 character=0 animation=0
LOC 83F6FF6F:11111111:0002012D PowerBeam amount=1 capacity=1 model=12345678 acs=00000000 character=0 animation=0
LOC 83F6FF6F:11111111:0002012E IceBeam amount=1 capacity=1 model=23456789 acs=00000000 character=0 animation=0
LOC 83F6FF6F:11111111:00020130 WaveBeam amount=1 capacity=1 model=3456789A acs=00000000 character=0 animation=0
LOC 83F6FF6F:11111111:0002012C Missiles amount=5 capacity=5 model=2D7E6590 acs=A9B8E446 character=0 animation=0
LOC 83F6FF6F:11111111:00020131 HealthRefill amount=10 capacity=0 model=FFFFFFFF acs=00000000 character=0 animation=0
LOC A8BE6291:22222222:00050010 EnergyTanks amount=1 capacity=1 model=ABCDEF01 acs=00000000 character=0 animation=0
LOC 39F2DE28:33333333:00040020 PowerSuit amount=1 capacity=1 model=FEDCBA98 acs=00000000 character=0 animation=0
"""
    with tempfile.TemporaryDirectory(prefix="make-ap-config-") as temp_dir:
        locations_path = os.path.join(temp_dir, "Locations.py")
        dump_path = os.path.join(temp_dir, "port.log")
        with open(locations_path, "w", encoding="utf-8") as output:
            output.write(source)
        with open(dump_path, "w", encoding="utf-8") as output:
            output.write(dump)

        ap_locations = read_ap_locations(locations_path)
        dump_locations, models = read_dumps([dump_path])
        result = join_locations(ap_locations, dump_locations, dict(WORLD_ASSET_IDS))
        expected = {
            "Chozo Ruins: A": "83F6FF6F:11111111:0002012C",
            "Chozo Ruins: B": "83F6FF6F:11111111:0002012D",
            "Chozo Ruins: C": "83F6FF6F:11111111:0002012E",
            "Chozo Ruins: D": "83F6FF6F:11111111:00020130",
            "Phendrana Drifts: E": "A8BE6291:22222222:00050010",
        }
        # D is the entry the dump has no counterpart for: the join must not
        # guess it (it becomes an --extra gap).
        mapped_expected = {name: key for name, key in expected.items() if name != "Chozo Ruins: D"}
        assert result["locations"] == mapped_expected, result["locations"]
        assert result["clean_areas"] == 1
        assert result["review_areas"] == 1
        assert result["mismatch_areas"] == 1
        assert result["missing_dump_areas"] == 1
        # The mixed-delta area keeps only the pairs at the dominant delta and
        # reports the rest instead of guessing them.
        assert result["unmapped"] == {
            "Chozo Ruins: D": 104,
            "Tallon Overworld: F": 301,
            "Tallon Overworld: G": 302,
            "Phazon Mines: H": 401,
        }, result["unmapped"]
        assert any(
            "no port counterpart at delta +1" in line and "Chozo Ruins: D" in line
            for line in result["report"]
        )
        assert any("dump 83F6FF6F:11111111:00020130 -> unpaired" in line for line in result["report"])
        assert any("count mismatch: Tallon Overworld area 0x0004" in line for line in result["report"])
        strict_out = os.path.join(temp_dir, "strict-should-not-exist.json")
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                main(
                    [
                        "--locations", locations_path,
                        "--dump", dump_path,
                        "--out", strict_out,
                        "--strict",
                    ]
                )
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError("--strict did not fail for a non-uniform area")
        assert not os.path.exists(strict_out), "--strict wrote output before failing"

        # Manual gap fillers: malformed keys are rejected, valid ones fill the
        # holes, and --strict still refuses while any AP location is unmapped.
        bad_extra = os.path.join(temp_dir, "bad-extra.json")
        with open(bad_extra, "w", encoding="utf-8") as output:
            output.write(json.dumps({"Chozo Ruins: D": "not-a-key"}))
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                main(
                    [
                        "--locations", locations_path,
                        "--dump", dump_path,
                        "--out", os.path.join(temp_dir, "bad.json"),
                        "--extra", bad_extra,
                    ]
                )
            except SystemExit as error:
                assert error.code == 2, error.code
            else:
                raise AssertionError("--extra must reject a malformed port key")

        partial_extra = os.path.join(temp_dir, "partial-extra.json")
        with open(partial_extra, "w", encoding="utf-8") as output:
            output.write(json.dumps({"Chozo Ruins: D": expected["Chozo Ruins: D"]}))
        partial_out = os.path.join(temp_dir, "partial.json")
        main(
            [
                "--locations", locations_path,
                "--dump", dump_path,
                "--out", partial_out,
                "--extra", partial_extra,
            ]
        )
        with open(partial_out, "r", encoding="utf-8") as source:
            partial = json.load(source)
        assert partial["locations"][expected["Chozo Ruins: D"]] == 104, partial["locations"]
        assert len(partial["locations"]) == 5

        with contextlib.redirect_stderr(io.StringIO()):
            try:
                main(
                    [
                        "--locations", locations_path,
                        "--dump", dump_path,
                        "--out", os.path.join(temp_dir, "strict2.json"),
                        "--extra", partial_extra,
                        "--strict",
                    ]
                )
            except SystemExit as error:
                assert error.code == 2, error.code
            else:
                raise AssertionError("--strict must still fail while AP locations are unmapped")

        full_extra = os.path.join(temp_dir, "full-extra.json")
        with open(full_extra, "w", encoding="utf-8") as output:
            output.write(
                json.dumps(
                    {
                        "Chozo Ruins: D": expected["Chozo Ruins: D"],
                        "Tallon Overworld: F": "39F2DE28:33333333:00040021",
                        "Tallon Overworld: G": "39F2DE28:33333333:00040022",
                        "Phazon Mines: H": "B1AC4D65:44444444:00070031",
                    }
                )
            )
        full_out = os.path.join(temp_dir, "full.json")
        main(
            [
                "--locations", locations_path,
                "--dump", dump_path,
                "--out", full_out,
                "--extra", full_extra,
                "--strict",
            ]
        )
        with open(full_out, "r", encoding="utf-8") as source:
            full = json.load(source)
        assert len(full["locations"]) == 8, full["locations"]

        arch = make_archipelago_config(result, "ws://localhost:38281", "Player1")
        assert arch["locations"] == {
            expected["Chozo Ruins: A"]: 101,
            expected["Chozo Ruins: B"]: 102,
            expected["Chozo Ruins: C"]: 103,
            expected["Phendrana Drifts: E"]: 201,
        }
        # The port supplies the world's item table itself.
        assert "items" not in arch
        assert arch["server"] == "ws://localhost:38281" and arch["slot"] == "Player1"

        spoiler = {
            "locations": {
                "Chozo Ruins: A": {"item": "Power Beam", "player": 1},
                "Chozo Ruins: B": {"item": "Missile Expansion", "player": 2},
                "Chozo Ruins: C": "Progressive Power Beam",
                "not in AP table": "Power Beam",
                "Phendrana Drifts: E": "Energy Tank",
            }
        }
        seed, skipped = make_randomizer_seed(spoiler, result["locations"], models)
        assert skipped == 1
        assert seed["locations"][expected["Chozo Ruins: A"]] == {
            "item": "PowerBeam", "amount": 1, "capacity": 1
        }
        assert seed["locations"][expected["Chozo Ruins: B"]] == {
            "item": "UnknownItem1", "amount": 0, "capacity": 0
        }
        assert seed["locations"][expected["Chozo Ruins: C"]]["item"] == "UnknownItem1"
        assert seed["locations"][expected["Phendrana Drifts: E"]]["item"] == "EnergyTanks"
        assert ITEMS_BY_NAME["Charge Beam (Ice)"] == ("ChargeBeam", 1, 1)
        assert seed["models"]["Missiles"]["model"] == "2D7E6590"

        # Exercise the JSON writer with temp paths while keeping self-test output
        # to the single documented success line.
        with contextlib.redirect_stdout(io.StringIO()):
            arch_path = os.path.join(temp_dir, "archipelago.json")
            seed_path = os.path.join(temp_dir, "randomizer_seed.json")
            write_json(arch_path, arch)
            write_json(seed_path, seed)
        with open(arch_path, "r", encoding="utf-8") as source_file:
            assert json.load(source_file)["locations"] == arch["locations"]
        with open(seed_path, "r", encoding="utf-8") as source_file:
            assert "models" in json.load(source_file)
    print("[make-ap-config-tests] passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locations", metavar="Locations.py", help="AP world's Locations.py")
    parser.add_argument(
        "--dump", metavar="LOG", action="append", help="port LOC dump (repeatable to merge)"
    )
    parser.add_argument("--out", metavar="FILE", help="write native AP client archipelago.json")
    parser.add_argument("--server", help="AP server URL to embed, e.g. ws://host:port")
    parser.add_argument("--slot", help="AP slot name to embed, e.g. Player1")
    parser.add_argument("--world-map", metavar="FILE.json", help="add/override world asset IDs")
    parser.add_argument("--extra", metavar="FILE.json", help="AP location name -> port key for gaps")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail unless every AP location is mapped (dump complete and joins clean)",
    )
    parser.add_argument("--spoiler", metavar="FILE.json", help="AP spoiler JSON")
    parser.add_argument("--seed-out", metavar="FILE", help="write port randomizer seed JSON")
    parser.add_argument("--slot-number", type=int, default=1, help="local AP player number (default: 1)")
    parser.add_argument("--seed-name", help="randomizer seed name (default: ap)")
    parser.add_argument("--self-test", action="store_true", help="run fast synthetic tests and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        if any(
            (
                args.locations,
                args.dump,
                args.out,
                args.server,
                args.slot,
                args.world_map,
                args.strict,
                args.spoiler,
                args.seed_out,
                args.seed_name,
            )
        ):
            parser.error("--self-test cannot be combined with generation options")
        try:
            _self_test()
        except Exception as error:
            print("[make-ap-config-tests] failed: %s" % error, file=sys.stderr)
            raise
        return 0

    if not args.locations or not args.dump or not args.out:
        parser.error("normal use requires --locations Locations.py, at least one --dump LOG, and --out FILE")
    if (args.spoiler is None) != (args.seed_out is None):
        parser.error("--spoiler and --seed-out must be given together")
    if args.seed_name and not args.spoiler:
        parser.error("--seed-name requires --spoiler and --seed-out")

    try:
        world_map = read_world_map(args.world_map)
        ap_locations = read_ap_locations(args.locations)
        dump_locations, models = read_dumps(args.dump)
        result = join_locations(ap_locations, dump_locations, world_map)
        extras = read_extra(args.extra)
        if extras:
            applied = apply_extra(result, extras, ap_locations)
            result["report"].append("file: %d gap(s) filled from --extra" % applied)
        _emit_report(result["report"])
        if args.strict and result["unmapped"]:
            sample = ", ".join(sorted(result["unmapped"])[:3])
            raise MakeAPConfigError(
                "--strict: %d AP location(s) have no port key (%s%s); no output written"
                % (
                    len(result["unmapped"]),
                    sample,
                    ", ..." if len(result["unmapped"]) > 3 else "",
                )
            )

        write_json(args.out, make_archipelago_config(result, args.server, args.slot))

        if args.spoiler:
            spoiler = _load_spoiler(args.spoiler)
            seed, skipped = make_randomizer_seed(
                spoiler,
                result["locations"],
                models,
                slot_number=args.slot_number,
                seed_name=args.seed_name or "ap",
            )
            write_json(args.seed_out, seed)
            if skipped:
                _emit_report(["spoiler: skipped %d location(s) with no joined port key" % skipped])
    except (OSError, json.JSONDecodeError, MakeAPConfigError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    main()
