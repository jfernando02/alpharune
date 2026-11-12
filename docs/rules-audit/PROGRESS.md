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

## Next
- Bundle: `bash /mnt/c/GitHub/riftbound-trainer/cloud/make_bundle.sh --engine /mnt/c/GitHub/alpharune --engine-ref claude/objective-jennings-a461e6`
- Rest of the 60 mismatches (moves always to base, counters see only the chain top, recall
  counts as a move, "first found" instead of a choice, single cards).
- Equipment triggers run inline instead of on the chain (fuzz-findings.md, Open).

## Audit agents
Batches in the scratchpad (batch1..9.txt). 1-3 done (verdicts1-3.tsv → cards.csv). 4-6 were
cut off by the usage limit (partial verdicts4-6.tsv may exist); 7-9 not started.
