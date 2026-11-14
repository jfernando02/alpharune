# Open rules questions for the user

One-line yes/no scenarios where the rules text left the engine a real choice. None from the
fuzzer; the card audit (349 cards) returned 2. All three are answered (user, 2026-11-14).

| # | Card / rule | Question | Answer | Engine now |
|---|---|---|---|---|
| 1 | Akshan, Mischievous (two "when you play me" abilities) | When Akshan is played with the additional cost paid, may the controller order the gear steal after the Weaponmaster equip (instead of before)? | Both happen (steal and Weaponmaster) | Unchanged: both happen, steal first. Verdict ok |
| 2 | Loyal Poro (718) | If Loyal Poro and the only other friendly unit at its battlefield die at the same time, does its Deathknell draw? | Yes: it didn't die alone | "Alone" is read as it died (last-known info); units dying at the same time count. Also fixes Lonely Poro (359, 777) |
| 3 | Death from Below (747) | Is its [A] replay from the trash made right away as the spell resolves, or is it a permission usable any time later that turn? | Right away | A "you may" as it resolves: pay [A], it goes on the chain again and chooses a new target |
