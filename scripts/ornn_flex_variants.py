#!/usr/bin/env python3
"""Ornn (Vendetta) flex-slot variants: the 31-card core stays, the 8 flex
slots are drawn from a pool of legal Calm / Mind cards, cheaper cards more
often (weight = BASE ** (energy + power cost)).

    python3 scripts/ornn_flex_variants.py --registry /tmp/reg.json --out decks/ornn/flex \
        --count 8 --seed 1

--registry is the engine's card table (riftbound --dump-registry FILE), used
for costs and to check every pool card exists. Writes one deck list per
variant plus the current list as flex_00_base.txt, and prints each variant's
flex cards and their average cost.
"""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

CORE = {  # 31 cards, never changed
    "Seal of Focus": 3, "Scuttle Crab": 3, "Patched Porobot": 3, "Clockwork Keeper": 3,
    "Sprite Fountain": 3, "Pit Crew": 3, "Brutalizer": 3, "Sterak's Gage": 3, "Poro Snax": 3,
    "Guardian Angel": 1, "Defy": 2, "Charm": 1,
}
BASE_FLEX = {  # the current 8
    "Legion Quartermaster": 1, "Aspiring Engineer": 1, "Defy": 1, "Charm": 1, "Retreat": 1,
    "Cloth Armor": 1, "Mask of Foresight": 1, "Guardian Angel": 1,
}
POOL = [
    # the current flex cards
    "Legion Quartermaster", "Aspiring Engineer", "Defy", "Charm", "Retreat", "Cloth Armor",
    "Mask of Foresight", "Guardian Angel",
    # Radiance
    "Ki Burst", "Life from Stone", "Hunker Down", "Consult the Heavens", "Command: Attack",
    "Parallel Convergence", "Iridescent Avian", "Invigorating Bloom", "High Note",
    "Improvised Projectile", "Ekko, Ingenious", "Riptide Mage",
    # older sets: interaction, protection, card flow, equipment, cheap bodies
    "Stupefy", "Mesmerize", "Rune Prison", "Smoke Screen", "Not So Fast", "Block", "Discipline",
    "Zhonya's Hourglass", "Doran's Shield", "Hand Hammer", "Meditation", "Back Off", "Eclipse",
    "Wages of Pain", "Frigid Touch", "Heart of Dark Ice", "Lecturing Yordle", "Apprentice Smith",
    "Mutated Mouser", "Crumbling Sands",
]
HEADER = "Legend:\n1 Ornn, Fire Below the Mountain\n\nChampion:\n1 Ornn, Blacksmith\n\nMainDeck:\n"
FOOTER = ("\nBattlefields:\n1 Ornn's Forge\n1 Seat of Power\n1 Veiled Temple\n\n"
          "Runes:\n8 Calm Rune\n4 Mind Rune\n")
FLEX_SLOTS = 8


def costs(registry: Path) -> dict[str, int]:
    data = json.loads(registry.read_text())
    cards = next(v for v in data.values() if isinstance(v, list))
    out: dict[str, int] = {}
    for c in cards:
        if c["name"] in out or c.get("banned"):
            continue
        out[c["name"]] = (c.get("energy_cost") or 0) + (c.get("power_cost") or 0)
    return out


def write(path: Path, flex: dict[str, int]) -> None:
    main = dict(CORE)
    for name, n in flex.items():
        main[name] = main.get(name, 0) + n
    assert sum(main.values()) == 39, path
    path.write_text(HEADER + "".join(f"{n} {name}\n" for name, n in main.items()) + FOOTER)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--registry", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--base", type=float, default=0.6, help="weight = BASE ** total cost")
    args = ap.parse_args()

    cost = costs(args.registry)
    missing = [n for n in POOL if n not in cost]
    if missing:
        raise SystemExit(f"not in the engine (or banned): {missing}")
    room = {n: 3 - CORE.get(n, 0) for n in POOL}
    weight = {n: args.base ** cost[n] for n in POOL}

    args.out.mkdir(parents=True, exist_ok=True)
    write(args.out / "flex_00_base.txt", BASE_FLEX)
    rng = random.Random(args.seed)
    seen = {tuple(sorted(BASE_FLEX.items()))}
    variants = []
    while len(variants) < args.count:
        flex: dict[str, int] = {}
        while sum(flex.values()) < FLEX_SLOTS:
            open_cards = [n for n in POOL if flex.get(n, 0) < room[n]]
            pick = rng.choices(open_cards, weights=[weight[n] for n in open_cards])[0]
            flex[pick] = flex.get(pick, 0) + 1
        key = tuple(sorted(flex.items()))
        if key in seen:
            continue
        seen.add(key)
        variants.append(flex)

    for i, flex in enumerate([BASE_FLEX] + variants):
        name = "flex_00_base" if i == 0 else f"flex_{i:02d}"
        if i > 0:
            write(args.out / f"{name}.txt", flex)
        avg = sum(cost[n] * k for n, k in flex.items()) / FLEX_SLOTS
        cards = ", ".join(f"{k} {n}" for n, k in sorted(flex.items(), key=lambda x: (cost[x[0]], x[0])))
        print(f"{name}  (avg cost {avg:.2f}): {cards}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
