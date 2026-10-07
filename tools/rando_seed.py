#!/usr/bin/env python3
"""Create item-randomizer seed JSON from location-dump logs.

Location keys use the C++ format ``%08X:%08X:%08X``: uppercase, zero-padded
eight-digit hexadecimal world asset ID, area asset ID, and entity/editor ID.

Dump lines produced with ``MP_RANDO_DUMP=1`` look like::

    LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5 \
        model=0A1B2C3D acs=00000000 character=0 animation=0

The model fields are optional so dumps from before model support still work;
without them the seed simply carries no models and rewritten pickups keep the
retail model.

``--from-dump`` shuffles the item multiset across the dumped locations (a true
1:1 shuffle: counts are preserved) and, alongside the placements, emits a
``models`` map derived from the dump. Every dumped item keeps the model it was
seen with, so a seed generated from a full dump draws every rewritten pickup as
the item it actually grants. A partial dump is still a valid seed: locations it
never saw keep their retail items, and items it never saw keep their retail
models.
"""

import argparse
import json
import random
import re


LOCATION_LINE = re.compile(
    r"^LOC ([0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}) "
    r"([A-Za-z0-9_]+) amount=(-?\d+) capacity=(-?\d+)"
    r"(?: model=([0-9A-Fa-f]{1,8}) acs=([0-9A-Fa-f]{1,8})"
    r" character=(\d+) animation=(\d+))?\s*$"
)


def write_json(path, data):
    with open(path, "w", encoding="utf-8") as output:
        json.dump(data, output, indent=2)
        output.write("\n")
    print(path)


def sample(path):
    write_json(
        path,
        {
            "seed": "sample",
            "locations": {
                "00000001:00000010:00000100": {
                    "item": "Missiles",
                    "amount": 5,
                    "capacity": 5,
                },
                "00000001:00000020:00000200": {
                    "item": "EnergyTanks",
                    "amount": 1,
                    "capacity": 1,
                },
            },
            "models": {
                "Missiles": {
                    "model": "01234567",
                    "acs": "00000000",
                    "character": 0,
                    "animation": 0,
                }
            },
        },
    )


def read_dump(log_path, locations, models, include_drops):
    with open(log_path, "r", encoding="utf-8") as source:
        for line in source:
            match = LOCATION_LINE.match(line.rstrip("\r\n"))
            if not match:
                continue
            key, item, amount, capacity, model, acs, character, animation = match.groups()
            amount = int(amount)
            capacity = int(capacity)
            # Areas list their enemy drop templates as pickups too (health and
            # ammo refills with no capacity). Only pickups that grant capacity
            # are real item locations, so those are the randomizer's targets.
            if capacity <= 0 and not include_drops:
                continue
            hidden = model is not None and int(model, 16) == 0xFFFFFFFF and int(acs, 16) == 0xFFFFFFFF
            locations[key.upper()] = (item, amount, capacity, hidden)
            if model is None or hidden:
                continue
            entry = {
                "model": "%08X" % int(model, 16),
                "acs": "%08X" % int(acs, 16),
                "character": int(character),
                "animation": int(animation),
            }
            previous = models.get(item)
            if previous is not None and previous != entry:
                print(
                    "warning: %s was seen with more than one model; keeping the first"
                    % item
                )
                continue
            models[item] = entry


def from_dump(log_paths, out_path, seed_name, shuffle_seed, include_drops):
    # Keep one entry per location key (using its last dump observation), since
    # the seed format is a mapping and cannot represent duplicate locations.
    locations = {}
    models = {}
    for log_path in log_paths:
        read_dump(log_path, locations, models, include_drops)

    if not locations:
        raise ValueError(
            "dump contains no item locations (capacity-granting pickups); "
            "use --include-drops to keep drop templates as well"
        )

    # A pickup with no model is one a script hands over unseen, next to a
    # visible pickup of the same item in the same area (Elite Quarters has two
    # Phazon Suits). It is not a location of its own: it follows its sibling's
    # placement, so the shuffle neither counts the item twice nor hides an item
    # where nobody can see it.
    followers = {}
    for key, (item, amount, capacity, hidden) in locations.items():
        if not hidden:
            continue
        area = key.rsplit(":", 1)[0]
        for other, (o_item, o_amount, o_capacity, o_hidden) in sorted(locations.items()):
            if (not o_hidden and other.startswith(area + ":")
                    and (o_item, o_amount, o_capacity) == (item, amount, capacity)):
                followers[key] = other
                break

    keys = sorted(key for key in locations if key not in followers)
    placements = [locations[key][:3] for key in keys]
    if shuffle_seed is None:
        random.shuffle(placements)
    else:
        random.Random(shuffle_seed).shuffle(placements)

    seed_locations = {}
    for key, (item, amount, capacity) in zip(keys, placements):
        seed_locations[key] = {"item": item, "amount": amount, "capacity": capacity}
    for key, leader in sorted(followers.items()):
        seed_locations[key] = dict(seed_locations[leader])

    seed = {"seed": seed_name, "locations": seed_locations}
    if models:
        seed["models"] = {item: models[item] for item in sorted(models)}
    write_json(out_path, seed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sample", metavar="OUT", help="write a small sample seed")
    mode.add_argument(
        "--from-dump",
        metavar="LOG",
        action="append",
        help="shuffle placements from a LOC dump (repeatable to merge dumps)",
    )
    parser.add_argument("--out", metavar="OUT", help="output seed path for --from-dump")
    parser.add_argument("--seed", metavar="NAME", help="seed name (default: randomized)")
    parser.add_argument("--shuffle-seed", metavar="N", type=int, help="make shuffling deterministic")
    parser.add_argument(
        "--include-drops",
        action="store_true",
        help="also treat zero-capacity enemy drop templates as locations",
    )
    args = parser.parse_args()

    if args.sample:
        if args.out or args.seed is not None or args.shuffle_seed is not None or args.include_drops:
            parser.error("--out, --seed, --shuffle-seed, and --include-drops apply only to --from-dump")
        sample(args.sample)
        return

    if not args.out:
        parser.error("--from-dump requires --out OUT")
    try:
        from_dump(args.from_dump, args.out, args.seed or "randomized", args.shuffle_seed,
                  args.include_drops)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
