# Rule-invariant fuzzer findings

Tool: `riftbound_rules_fuzz` (`tools/rules_fuzz.cpp`). Each finding names the check that caught
it, the cause, and the fix with its test. "Not a rule bug" entries are checker noise that was
fixed in the checker.

## Rule bugs (fixed)

| # | Check | Cards | Bug | Fix / test |
|---|---|---|---|---|
| 1 | zone | Fizz, Trickster; Kai'Sa, Evolutionary (any "recycle") | `recycleCards` left the card in the list it came from: a spell recycled from the chain/trash stayed in the trash AND went to the deck (a free extra copy) | `EffectExecutor::recycleCards` removes the card from every list; `ZoneIntegrityTest.RecyclingTakesTheCardOutOfItsList`, `ThenRecycleItPutsTheSpellOnlyInTheDeck` |
| 2 | zone | any token recycled | a recycled token went into the deck (CR 183.1: it ceases to exist) | same; `ZoneIntegrityTest.RecycledTokenCeasesToExist` |
| 3 | zone | Time Warp, Arcane Shift | a spell that banishes itself while resolving was also put into the trash | chain disposal only when the spell is still on the chain; `ZoneIntegrityTest.SelfBanishingSpellIsNotAlsoTrashed` |
| 4 | zone | Keeper's Verdict | a token put on top/bottom of the deck became a deck card | token ceases to exist; `ZoneIntegrityTest.KeepersVerdictOnATokenRemovesIt` |
| 5 | winner below victory | any (e.g. Ahri, Alluring's Beginning-Step point) | no Cleanup after each chain item (CR 319.5), so a player who reached the Victory Score mid-chain played on and later items still resolved | `ChainManager` runs cleanup after each removed item and stops on game over; `ChainCleanupTest.CleanupAfterEachItemAndNothingResolvesAfterAWin` |
| 6 | activation (repeat) | Grandmaster at Arms (legend) | could "attach" an Equipment from anywhere / repeat with nothing to attach | only Equipment on the board; ability not offered without one; `AbilityGates.GrandmasterAttachesOnlyEquipmentOnTheBoard` |
| 7 | zone | every [Weaponmaster] unit (Lucian, Merciless; Ornn, Forge God; Jax, Unrelenting; Yone, Blademaster; Sentinel Adept; Armed Assailant; Riven, Shattered; …) | CR 821 not implemented: always took the first Equipment (no "you may", no choice), paid nothing (an Equip cost of [1][R] or "[P], Recycle 2" was free), and attached to the Weaponmaster even after it died (gear attached to a unit in the trash) | one CR 821 implementation (`resolveWeaponmaster`, `Card::weaponmasterEquip` per Equipment); choices at finalization; `WeaponmasterTest.*` |
| 8 | activation (repeat) / no-op | Sett, Brawler (3 decks); Udyr, Wildman | "Spend my buff" was taken at resolution and the gate asked whether ANY Sett its controller had was buffed: an unbuffed copy activated without end (1,994 times in one game) | `ActivationCost::spend_buff_self`, paid at activation, gated per object; `AbilityGatesTest.SpendMyBuffIsPaidByThisUnitAtActivation` |
| 9 | (same class, by code search) | Renata Glasc, Mastermind (1 deck); Caitlyn, Patrolling; Xerath, Freed; Ultrasoft Poro; Maduli, the Gatekeeper; Cursed Sarcophagus | ability gate checked "any copy of this card I control" instead of the activating object | `canActivateAbilityAt` per object |
| 10 | zone | Azir, Ascendant (1 deck) | killed in response to his own swap ability, he was "moved" out of the trash onto the board | `moveToBase` / `moveToBattlefield` only move objects on the board; `ZoneIntegrityTest.MovingAnObjectThatLeftTheBoardDoesNothing` |
| 11 | longturn / no-op | Hextech Gauntlets (1 deck) | Equip cost depends on the chosen unit's Might, but every unit was offered once any unit made it affordable; the failed equip repeated (495 times) | `Card::canEquipTo`; Gauntlets pay through the shared equip payment; `AbilityGatesTest.EquipWithAUnitDependentCostOffersOnlyPayableUnits` |
| 12 | no-op / state | Fading Memories, Star-Crossed (any non-[Reaction] spell) | the turn was Open while a chain existed (a Cleanup or a Showdown ending between chain items reopened it): a normal-timing spell was offered at chain priority as a plain play, which the chain ignores -- chosen again and again | Closed while a chain exists (CR 309.1) in the chain loop and the Showdown transitions; `ChainCleanupTest.TheTurnStaysClosedWhileAChainExists`, `AShowdownEndingWithAChainLeavesTheTurnClosed` |
| 13 | (card audit, then usage) | Akshan, Mischievous (2 decks) | no [Weaponmaster] at all, and "move an enemy gear" took the first enemy gear | both abilities in one trigger, the controller chooses the gear; `WeaponmasterTest.AkshanLetsTheControllerChooseTheEnemyGear`, `AkshanIsAWeaponmaster` (99d7f0e) |
| 14 | state | any trigger from a Beginning-Step Cleanup | triggers added by the start-of-turn Cleanup sat on the chain into the Main Phase (CR 309.1) | `cleanupAndRunChain` in the Beginning and Scoring Steps; `BurnOutScoringTest.ATriggerFromTheBeginningStepCleanupResolvesBeforeTheMainPhase` (1ac5c14) |
| 15 | score [past victory] | Last Rites (Miss Fortune, Annie, Sivir decks) | scoring emitted its event before the win check, and equipment triggers run inline on it: after Last Rites began charging costs (group 1) a rune payment was asked after the game was won (CR 467) | win check before the score event; no inline equipment trigger once the game is over; `CostPaymentRulesTest.NothingIsAskedAfterTheWinningHold` (9855f01) |
| 16 | longturn | Rengar, Pouncing | in a combat showdown the pounce is offered as `PlayReaction`, which `executeIntent` did not handle: a no-op chosen 100+ times in a turn | `executeIntent` plays `PlayReaction`; `CostPaymentRulesTest.APounceInAShowdownPlaysTheUnit` (9855f01) |

## Screens added per shared cause

Checks in `riftbound_rules_fuzz` that would have caught each general bug the card audit found:
- `choice [trigger target]`: a triggered ability of a non-spell card that declares a target is
  finalized with none while a legal one exists (25 abilities did nothing; 5505683).
- `choice [hidden target]`: a spell / play effect of a card played from facedown chose an object
  away from that battlefield (CR 811.1.d).

## Checker noise (fixed in the checker, not rule bugs)

- Battlefield card objects have no location: exempt.
- Zone and conservation checks only at settled decision points (a cost being paid or an item
  resolving moves cards between lists in steps).
- Granted abilities share an ability index with the card's own: keyed by granted_ability_def.
- Victory score includes battlefield modifiers and "more points than the opponent".
- Burn-out point had no scoring event: the engine now emits PointsGainedEvent (commit 24431ee).
- Rune pool emptiness checked when the Main Phase begins (CR 167), not when it resumes after
  combat (Gold's [A] added during the opponent's combat may be held until end of turn).
- Copy check compares deck contents, not deck order: a resumed copy has its own engine RNG
  (not part of GameState), so a random reorder legitimately differs.

## Runs (release build, agents random / scripted / policy, all deck pairs)

| seed | games | decisions | violations after fixes |
|---|---|---|---|
| 2 | 1,521 | ~290k | found 1-7 and the Fiora/Chem-Baroness long turn; not re-run after fixes 8-12 yet |
| 3 | 1,521 | 282,691 | 0 |
| 5 | 3,042 | 567,316 | 0 |
| 6 (every 3rd clean point: all legal intents on copies, 255k) | 1,521 | 287,989 | 0 |
| 7, asserts + ASan + UBSan build (every 10th clean point: 16,172 intents on copies) | 300 | 57,879 | 0; no sanitizer report, no assert |

The ASan run also lists units seen ready right after entering the board. Nearly all are
[Accelerate] units and tokens played ready (Sprite, Recruit, Reflection, Shadow). Singletons
not yet traced: Vex, Apathetic; Blue Sentinel; Annie, Stubborn; Ahri, Alluring (most likely readied
by another effect before the next decision).

## Open (found, not fixed yet)

- Equipment triggered abilities ("When I hold / conquer / die" on Last Rites and others) run
  inline when the event happens instead of going on the chain (CR 383), so they can't be
  responded to and resolve before other triggers. Not fixed yet.
- Equipment whose Weaponmaster cost needs a choice are not offered to Weaponmaster:
  Blade of the Ruined King ("kill a friendly unit"), Shady Spectacles (copy choice while
  attaching). Neither is in a training deck.
