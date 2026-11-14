# Rules audit — progress

Branch `claude/objective-jennings-a461e6` (not pushed). Order agreed with the user:
exploitable/free value → shared causes #1 (trigger targets) and #2 (Hidden plays) → bundle
rebuild → rest of the first 117 cards → automatic screens per shared cause → audit of the
remaining 238 cards (subagents read; a mismatch counts once a test fails on it).

## Done
- A scope.csv (355 cards, all in at least one training deck).
- B fuzzer (`tools/rules_fuzz.cpp`): bugs 1-16 in fuzz-findings.md.
- C usage.csv (3,042 policy-vs-policy games) + anomalies.csv.
- D printed text vs class text: every difference is official errata (errata/). Behavior audit
  of the 117 cards in 3+ decks: 60 mismatches (cards.csv, `status` column).

## Commits
- 94e771b group 1, free value: [Repeat] once (CR 820.1.c.3); conditional keywords no longer
  printed on 13 cards; aura/Equipment [Assault]/[Shield] add Might; Gold from triggers enters
  exhausted; Last Rites charges costs; Shadow's Call, Sacrifice, Deadly Flourish, Confront.
- 5505683 shared cause #1: triggered abilities choose their target as they go on the chain
  (25 abilities did nothing); "here"/"another" per card; Tideturner, Beast Below, Yuumi,
  Blast Cone, Amateur Recital, Veiled Temple.
- 9855f01 found by the fuzzer after those: win check before score triggers (Last Rites asked
  for a payment after the game was won); a showdown pounce (Rengar) was a repeatable no-op.
- acf2240 shared cause #2: cards played from facedown (CR 811): targets at the hidden
  battlefield for spells and play effects (Hidden Blade, Back Off, Switcheroo, Blastcone Fae);
  playable at any [Reaction] timing, not only Closed States; the real play path now announces
  "played from face down" (Katarina, Evelynn, Tornado Warrior never saw it); fuzzer screens
  `choice [trigger target]` and `choice [hidden target]`.

- 45c7c38 the facedown restriction left cards in hand / battlefield cards alone (Bone Skewer).
- 7f3382c the user's rulings on the 3 open questions: "alone" is read as a unit died, and
  friends dying at the same time count (Loyal Poro, Lonely Poro x2); Death from Below's replay
  is made right away; Akshan unchanged (both happen). Fuzz seed 2 + usage seed 9: 0 violations.

## Bundle
`/mnt/c/GitHub/riftbound-trainer/dist/riftbound-cloud-20261112.tar.gz` (22M), engine 6d480a0,
trainer 6a6f292. Fuzz after the last engine change: seed 2 (1,521 games) and usage seed 9
(3,042 policy games) at 0 violations, including the two new choice checks.

## Card audit (step D) — all 349 non-rune cards in scope
182 ok, 167 mismatch, 0 unsure. 19 mismatches fixed so far. Open ones by kind:
printed keywords 13, unfiltered cost reductions 3, "first found" instead of a choice 37,
always to base 20, forced "may" 7, counters see only the chain top 4, missing clauses 23,
other 42.

## Next
- Free value among the new mismatches: printed keywords (Gem Jammer, Megatusk, Chakram
  Dancer, Frisky Hunter, Breakneck Mech; Temporary on Sprite Mother / Trevor / Lillia),
  unfiltered cost reductions (Eager Apprentice, Herald of Scales, Vaults of Helia),
  Chem-Baroness, Glasc Mixologist, Eye of the Herald, Viktor, Keeper of the Hammer, Leona
  Zealot; plus a test that every printed keyword is printed in the text.
- Then shared causes: "first found" -> choice, moves to base, chain-top counters, two-target
  generation, [Reaction] units, Equipment triggers inline, gear deaths.
