#include <cstdio>
#include <cstdlib>
#include "game_engine.h"
#include "cards/card.h"
#include "openspiel/action_vocab.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace riftbound {

// ═══════════════════════════════════════════════════════════════════════════════
// Legal action generation
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<Intent> GameEngine::generateLegalActions() const {
    auto player = state_.turn.turn_player;

    // In Closed State, the priority holder acts (not necessarily turn player)
    if (state_.turn.isClosedState() && state_.turn.priority_holder.has_value()) {
        return generateClosedStateActions(*state_.turn.priority_holder);
    }

    switch (state_.turn.phase) {
        case TurnPhase::Mulligan:
            return generateMulliganActions(player);
        case TurnPhase::MainPhase:
            if (state_.turn.isNeutralOpen()) {
                return generateMainPhaseActions(player);
            }
            if (state_.turn.isShowdownOpen()) {
                // Focus holder acts in showdowns
                if (state_.turn.focus_holder.has_value()) {
                    return generateShowdownActions(*state_.turn.focus_holder);
                }
                return generateShowdownActions(player);
            }
            break;
        default:
            break;
    }

    return {};
}

std::vector<std::pair<GameObjectId, Intent::PlaySource>>
GameEngine::playableCards(PlayerId player) const {
    const auto& ps = state_.player(player);
    std::vector<std::pair<GameObjectId, Intent::PlaySource>> out;
    for (auto id : ps.hand) out.push_back({id, Intent::PlaySource::Hand});
    // "You may play cards from your trash." (Endless Riches). A trash card with
    // its own replay grant ([Flow] etc.) is offered by generateTrashReplayActions.
    if (ps.may_play_from_trash)
        for (auto id : ps.trash) {
            bool granted = false;
            for (const auto& g : ps.trash_replay_grants) granted = granted || g.card == id;
            if (!granted && state_.objectExists(id) && state_.getObject(id).zone == ZoneType::Trash)
                out.push_back({id, Intent::PlaySource::Trash});
        }
    // "Until your next Ending Phase, you may play that card." (Runeblade Anvil)
    for (const auto& g : ps.banish_play_grants)
        if (g.until_turn >= state_.turn.turn_number && state_.objectExists(g.card) &&
            state_.getObject(g.card).zone == ZoneType::Banishment)
            out.push_back({g.card, Intent::PlaySource::Banishment});
    return out;
}

std::vector<Intent> GameEngine::generateMainPhaseActions(PlayerId player) const {
    std::vector<Intent> actions;

    // Always can end turn
    actions.push_back(Intent::endTurn(player));

    // Standard Move: exhaust ready units to move them. A move here may carry
    // a cost (Gravity Field); only offered when it can be paid.
    auto move_ok = [&](const LocationId& d) {
        const auto c = moveCost(player, d, 1);
        return canPayAnyDomainCost(player, c.first, c.second);
    };
    auto all_units = state_.allUnitsControlledBy(player);
    for (auto unit_id : all_units) {
        auto& unit = state_.getObject(unit_id);
        if (unit.is_exhausted) continue; // must be ready
        if (unit.cant_move_on_turn == state_.turn.turn_number) continue;  // "can't move this turn"

        if (unit.isAtBase()) {
            // Can move from base to any battlefield (CR 144.4.a).
            // Rockfall Path's "can't be PLAYED here" does NOT restrict
            // movement — only the play-from-hand action generators
            // consult blocks_unit_play.
            for (auto& bf : state_.battlefields) {
                // Can't move to BF with units from 2 other players (CR 144.4.a.1)
                // In 1v1, this is never an issue
                LocationId dest = BattlefieldLocation{bf.id};
                if (!move_ok(dest)) continue;
                actions.push_back(Intent::standardMove(
                    player, {unit_id}, dest));
            }
        } else if (unit.isAtBattlefield()) {
            // Can normally move back to base (CR 144.4.b). Vilemaw's Lair
            // and similar locking BFs set `blocks_move_to_base` to deny
            // this leg.
            auto unit_bf = unit.battlefieldId();
            bool allow_to_base = true;
            if (unit_bf) {
                const auto& src_bf = getBattlefield(*unit_bf);
                if (src_bf.blocks_move_to_base || src_bf.aura_blocks_move_to_base) allow_to_base = false;
            }
            if (unit.cant_move_to_base) allow_to_base = false;  // Determined Sentry
            if (allow_to_base) {
                actions.push_back(Intent::standardMove(
                    player, {unit_id}, BaseLocation{player}));
            }

            // BF→BF moves allowed when EITHER:
            //   - unit has Ganking (CR 144.4.c), OR
            //   - destination BF has `accepts_any_inbound` (Baron Pit's
            //     "Units can move here from anywhere" rule).
            // Either route emits one move per legal destination.
            // Rockfall Path's "can't be played here" does NOT restrict
            // BF→BF moves; only play-from-hand checks blocks_unit_play.
            const bool has_ganking = unit.hasKeyword(Keyword::Ganking);
            for (auto& bf : state_.battlefields) {
                if (!unit_bf || *unit_bf == bf.id) continue;
                const bool can_move = has_ganking || bf.accepts_any_inbound;
                if (!can_move || !move_ok(BattlefieldLocation{bf.id})) continue;

                actions.push_back(Intent::standardMove(
                    player, {unit_id}, BattlefieldLocation{bf.id}));
            }
        }
    }

    // Play units from hand — must be able to afford the cost.
    auto& ps = state_.player(player);
    const bool locked_out = ps.cant_play_cards_this_turn;  // Brynhir Thundersong
    for (const auto& [card_id, src] : playableCards(player)) {
        if (locked_out) break;  // skip all play-from-hand actions
        PlaySourceStamp stamp{actions, actions.size(), src};
        auto& card = state_.getObject(card_id);
        if (!card.isUnit()) continue;
        if (card.card_def_id == kInvalidId) continue;  // tokens have no CardDef
        // Affordable as printed, or only where a battlefield discounts it
        // (Hunters' Circle).
        const bool affordable = canAfford(player, card_id) ||
                                affordableWithAdditionalCost(player, card_id);
        auto affordable_at = [&](BattlefieldId b) {
            if (affordable) return true;
            const int d = playHereDiscount(player, b, card);
            return d > 0 && canAfford(player, card_id, 0, d);
        };
        if (!affordable) {
            bool any = false;
            for (const auto& bf : state_.battlefields) any = any || affordable_at(bf.id);
            if (!any) continue;
        }

        // Check play-to-location abilities from ability_text
        const auto& def = card_db_.get(card.card_def_id);
        auto& text = def.ability_text;
        bool can_play_open_bf = (text.find("play me to an open battlefield") != std::string::npos);
        bool can_play_enemy_bf = (text.find("play me to an occupied enemy battlefield") != std::string::npos);
        bool can_play_any_bf = (text.find("play me to a battlefield") != std::string::npos ||
                                text.find("play me at a battlefield") != std::string::npos);
        // Board-wide grant: a friendly unit (e.g. Miss Fortune, Buccaneer) lets
        // ALL of this player's units be played to open battlefields.
        if (ps.grant_friendly_units_open_bf) can_play_open_bf = true;
        // "The next unit you play this turn can be played to any battlefield." (Last Caress)
        if (ps.next_unit_any_bf) can_play_any_bf = true;

        Intent play_intent;
        play_intent.type = IntentType::PlayCard;
        play_intent.player = player;
        play_intent.card = card_id;

        // NARROWING hook (Perched Grimwyrm): the card dictates its own legal
        // play locations and suppresses the default base + BF plays entirely.
        const Card* card_obj = card_registry_.get(card.card_def_id);
        if (card_obj && card_obj->restrictsPlayLocations()) {
            for (const auto& loc : card_obj->getPlayLocations(state_, player)) {
                if (const auto* at = std::get_if<BattlefieldLocation>(&loc)) {
                    if (!affordable_at(at->id)) continue;
                } else if (!affordable) {
                    continue;
                }
                Intent restricted = play_intent;
                restricted.play_location = loc;
                actions.push_back(restricted);
            }
            continue;  // no default base/BF plays for this card
        }

        // Can always play to base (CR 355.2.a)
        play_intent.play_location = BaseLocation{player};
        if (affordable) actions.push_back(play_intent);

        // Mageseeker Warden: while it restricts this player, units may be
        // played ONLY to base — skip all battlefield plays.
        if (ps.units_play_base_only) continue;

        for (auto& bf : state_.battlefields) {
            if (bf.blocks_unit_play) continue;  // Rockfall Path
            bool controlled = bf.controller.has_value() && *bf.controller == player;
            bool open = !bf.controller.has_value();
            bool enemy_occupied = bf.controller.has_value() && *bf.controller != player &&
                                  bf.hasUnitsFrom(opponent(player), state_.objects);

            bool can_play_here = controlled; // default: controlled BFs
            if (can_play_open_bf && open) can_play_here = true;
            if (can_play_enemy_bf && enemy_occupied) can_play_here = true;
            if (can_play_any_bf) can_play_here = true;

            if (can_play_here && affordable_at(bf.id)) {
                Intent bf_play = play_intent;
                bf_play.play_location = BattlefieldLocation{bf.id};
                actions.push_back(bf_play);
            } else if (!can_play_here && affordable && bf.paid_play_power > 0 &&
                       std::find(card.tags.begin(), card.tags.end(), bf.paid_play_tag) !=
                           card.tags.end() &&
                       canAfford(player, card_id, bf.paid_play_power)) {
                // "Any player may pay ... as an additional cost to play a <tag>.
                // If they do, they play it to this battlefield."
                Intent paid_play = play_intent;
                paid_play.play_location = BattlefieldLocation{bf.id};
                paid_play.pay_location_cost = true;
                actions.push_back(paid_play);
            }
        }
    }

    // Play champion from champion zone (also must afford)
    if (ps.champion_zone != kInvalidId) {
        auto& champ = state_.getObject(ps.champion_zone);
        if (champ.zone == ZoneType::ChampionZone) {
            if (canAfford(player, ps.champion_zone)) {
                Intent play_intent;
                play_intent.type = IntentType::PlayCard;
                play_intent.player = player;
                play_intent.card = ps.champion_zone;
                play_intent.play_location = BaseLocation{player};
                actions.push_back(play_intent);
            }
            // Alternative play cost (Jhin, Meticulous Killer): offer "play me for
            // <alt>" when the condition holds and the alt cost is affordable —
            // independent of whether the printed cost is affordable.
            const Card* cc = champ.card_def_id != kInvalidId
                ? card_registry_.get(champ.card_def_id) : nullptr;
            if (cc) {
                auto alt = cc->alternativePlayCost(state_, player);
                bool afford = false;
                if (alt.valid) {
                    if (alt.any_domain) {
                        for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
                            if (canPayAdditionalCost(player, alt.energy, alt.power,
                                                      static_cast<Domain>(di),
                                                      state_.purposePlaying(CardType::Unit))) {
                                afford = true; break;
                            }
                    } else {
                        afford = canPayAdditionalCost(player, alt.energy, alt.power,
                                                       alt.power_domain,
                                                       state_.purposePlaying(CardType::Unit));
                    }
                }
                if (afford) {
                    Intent alt_play;
                    alt_play.type = IntentType::PlayCard;
                    alt_play.player = player;
                    alt_play.card = ps.champion_zone;
                    alt_play.play_location = BaseLocation{player};
                    alt_play.use_alt_play_cost = true;
                    actions.push_back(alt_play);
                }
            }
        }
    }

    // Play gear from hand (to base only, CR 149.2)
    for (const auto& [card_id, src] : playableCards(player)) {
        if (locked_out) break;
        PlaySourceStamp stamp{actions, actions.size(), src};
        auto& card = state_.getObject(card_id);
        if (!card.isGear()) continue;
        const bool affordable = canAfford(player, card_id);

        Intent play_intent;
        play_intent.type = IntentType::PlayCard;
        play_intent.player = player;
        play_intent.card = card_id;
        // [Deploy] (CR 830.1.b): only to a battlefield, any battlefield.
        if (card.card_def_id != kInvalidId && card_db_.get(card.card_def_id).deploy) {
            for (const auto& bf : state_.battlefields) {
                if (!affordable) {   // a battlefield's discount (Hunters' Circle)
                    const int d = playHereDiscount(player, bf.id, card);
                    if (d <= 0 || !canAfford(player, card_id, 0, d)) continue;
                }
                Intent to_bf = play_intent;
                to_bf.play_location = BattlefieldLocation{bf.id};
                actions.push_back(to_bf);
            }
            continue;
        }
        if (!affordable) continue;
        play_intent.play_location = BaseLocation{player};
        actions.push_back(play_intent);
    }


    // Play spells from hand — Neutral Open allows any spell (Action or not).
    // Brynhir lockout suppresses card plays but not activated abilities.
    if (!locked_out) {
        generateSpellActions(player, /*action_ok=*/true, /*reaction_ok=*/true,
                              actions);
        generateTrashReplayActions(player, /*action_ok=*/true, /*reaction_ok=*/true,
                                    actions);
        appendHiddenPlays(player, actions);   // CR 811.6
    }

    // Activate abilities on gear/legends/units ([E]: abilities)
    generateActivateAbilityActions(player, actions);

    // Equip gear to units (CR 818). Phase 6r — if a gear opts into
    // needsEquipTimeTarget, emit ONE intent (no target) instead of one
    // per (gear, friendly_unit) pair. The engine's equip-execution
    // path calls onEquip(ctx, kInvalidId) and the gear's Card uses
    // pickTarget inside onEquip to resolve the target. Fixes the same
    // action-vocab collision class as needsPlayTimeTarget /
    // needs_activation_time_target for gear equip intents.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isGear() || obj.controller != player) continue;
        if (!obj.location.has_value()) continue;
        if (obj.attached_to.has_value()) continue; // already attached

        Card* gear_card = card_registry_.get(obj.card_def_id);
        if (!gear_card || !gear_card->hasEquipAbility()) continue;
        const bool usual_cost = gear_card->canEquip(state_, player);
        // "[Equip] — [A] or exhaust a friendly unit": the ready friendly units
        // that could be exhausted instead (Card::equipByExhaustingAUnit).
        std::vector<GameObjectId> exhaustable;
        if (gear_card->equipByExhaustingAUnit()) {
            for (const auto& [xid, x] : state_.objects)
                if (x.isUnit() && x.controller == player && x.location.has_value() &&
                    !x.is_exhausted)
                    exhaustable.push_back(xid);
        }
        if (!usual_cost && exhaustable.empty()) continue;
        if (!exhaustable.empty()) {
            for (auto& [uid, unit] : state_.objects) {
                if (!unit.isUnit() || unit.controller != player) continue;
                if (!unit.location.has_value()) continue;
                for (auto xid : exhaustable) {
                    Intent equip;
                    equip.type = IntentType::ActivateAbility;
                    equip.player = player;
                    equip.ability_source = id;
                    equip.targets = {uid, xid};   // equip uid, exhaust xid to pay
                    actions.push_back(equip);
                }
            }
        }
        if (!usual_cost) continue;

        if (gear_card->needsEquipTimeTarget()) {
            // One intent per gear; target chosen at resolve via pickTarget.
            Intent equip;
            equip.type = IntentType::ActivateAbility;
            equip.player = player;
            equip.ability_source = id;
            actions.push_back(equip);
            continue;
        }

        // Legacy path: enumerate friendly units as equip targets (all
        // unmigrated gear). Per-target action_vocab slot collision lives
        // here until the gear opts into needsEquipTimeTarget.
        for (auto& [uid, unit] : state_.objects) {
            if (!unit.isUnit() || unit.controller != player) continue;
            if (!unit.location.has_value()) continue;
            if (!gear_card->canEquipTo(state_, player, uid)) continue;

            Intent equip;
            equip.type = IntentType::ActivateAbility;
            equip.player = player;
            equip.ability_source = id;
            equip.targets = {uid};
            actions.push_back(equip);
        }
    }

    // Hide cards with [Hidden] keyword facedown at controlled battlefields (CR 811)
    for (auto card_id : ps.hand) {
        auto& card = state_.getObject(card_id);
        if (!card.keywords.has(Keyword::Hidden)) continue;

        // Need at least one rune to pay [A] cost
        bool has_rune = false;
        auto base_loc = BaseLocation{player};
        for (auto& [id, obj] : state_.objects) {
            if (obj.isRune() && obj.controller == player && !obj.is_exhausted &&
                obj.location.has_value() && *obj.location == LocationId{base_loc}) {
                has_rune = true;
                break;
            }
        }
        if (!has_rune && !ps.hide_ignoring_costs_this_turn &&
            !(ps.hide_for_energy && canPayAdditionalCost(player, 1, 0, Domain::Fury, SpendPurpose{})))
            continue;

        // Can hide at any controlled BF with available facedown slot
        for (auto& bf : state_.battlefields) {
            if (!bf.controller.has_value() || *bf.controller != player) continue;
            if (static_cast<int>(bf.facedown.size()) >=
                bf.facedown_max_occupancy + bf.extra_hide_slots) continue;

            Intent hide;
            hide.type = IntentType::HideCard;
            hide.player = player;
            hide.card = card_id;
            hide.chosen_battlefield = bf.id;
            actions.push_back(hide);
        }
    }

    return actions;
}

std::vector<Intent> GameEngine::generateMulliganActions(PlayerId player) const {
    std::vector<Intent> actions;
    auto& ps = state_.player(player);

    // Can mulligan 0 cards (keep hand)
    actions.push_back(Intent::mulligan(player, {}));

    // Can mulligan 1 card
    for (auto card_id : ps.hand) {
        actions.push_back(Intent::mulligan(player, {card_id}));
    }

    // Can mulligan 2 cards
    for (size_t i = 0; i < ps.hand.size(); ++i) {
        for (size_t j = i + 1; j < ps.hand.size(); ++j) {
            actions.push_back(Intent::mulligan(
                player, {ps.hand[i], ps.hand[j]}));
        }
    }

    return actions;
}

std::vector<Intent> GameEngine::generateShowdownActions(PlayerId player) const {
    std::vector<Intent> actions;

    // Can always pass focus
    actions.push_back(Intent::passFocus(player));

    const bool locked_out = state_.player(player).cant_play_cards_this_turn;

    // Can play Action or Reaction spells during showdowns (CR 806, 813).
    // Lockout (Brynhir Thundersong) suppresses card plays but not abilities.
    if (!locked_out) {
        generateSpellActions(player, /*action_ok=*/true, /*reaction_ok=*/true,
                              actions);
        generateTrashReplayActions(player, /*action_ok=*/true, /*reaction_ok=*/true,
                                    actions);
        appendHiddenPlays(player, actions);   // CR 811.6
    }

    // Ambush: play units with [Ambush] during showdowns
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isUnit() || !card.hasKeyword(Keyword::Ambush)) continue;
        if (!canAfford(player, card_id)) continue;

        const Card* amb = card.card_def_id != kInvalidId
            ? card_registry_.get(card.card_def_id) : nullptr;
        const bool enemy_bf_ok = amb && amb->ambushToEnemyBattlefields();
        for (auto& bf : state_.battlefields) {
            if (bf.blocks_unit_play) continue;  // Rockfall Path
            bool here = bf.hasUnitsFrom(player, state_.objects);
            // Rengar, Trophy Hunter: may Ambush to a BF with enemy units even
            // without units of your own there.
            if (!here && enemy_bf_ok)
                here = bf.hasUnitsFrom(opponent(player), state_.objects);
            if (!here) continue;
            Intent ambush;
            ambush.type = IntentType::PlayActionCard;
            ambush.player = player;
            ambush.card = card_id;
            ambush.play_location = BattlefieldLocation{bf.id};
            actions.push_back(ambush);
        }
    }

    // Gear with [Action] (granted: Jarvan IV's "Your gear with [Deploy] have
    // [Action]") can be played in showdowns.
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isGear() || !card.hasKeyword(Keyword::Action)) continue;
        if (card.card_def_id == kInvalidId || !canAfford(player, card_id)) continue;
        Intent play;
        play.type = IntentType::PlayActionCard;
        play.player = player;
        play.card = card_id;
        if (card_db_.get(card.card_def_id).deploy) {
            for (const auto& bf : state_.battlefields) {
                Intent to_bf = play;
                to_bf.play_location = BattlefieldLocation{bf.id};
                actions.push_back(to_bf);
            }
        } else {
            play.play_location = BaseLocation{player};
            actions.push_back(play);
        }
    }

    // Reaction-to-attack plays (e.g., Rengar, Pouncing — play to a BF where
    // you are currently attacking).
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isUnit()) continue;
        if (card.card_def_id == kInvalidId) continue;
        Card* card_obj = card_registry_.get(card.card_def_id);
        if (!card_obj || !card_obj->playableAsReactionToAttack()) continue;
        if (!canAfford(player, card_id)) continue;

        for (auto& bf : state_.battlefields) {
            if (bf.blocks_unit_play) continue;  // Rockfall Path
            if (!bf.combat_in_progress) continue;
            if (!bf.attacker.has_value() || *bf.attacker != player) continue;
            Intent pounce;
            pounce.type = IntentType::PlayReaction;
            pounce.player = player;
            pounce.card = card_id;
            pounce.play_location = BattlefieldLocation{bf.id};
            actions.push_back(pounce);
        }
    }

    // Can activate [E]: abilities with [Action] timing during showdowns (CR 806)
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != player) continue;
        if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;

        Card* card = card_registry_.get(obj.card_def_id);
        if (!card) continue;
        auto abilities = card->activatedAbilities();
        for (size_t ai = 0; ai < abilities.size(); ++ai) {
            const auto& ab = abilities[ai];
            if (!ab.is_action) continue;
            // The same gates as the main phase: once-per-turn and other
            // "use only if" limits, a ready source for an exhaust cost,
            // affordability and target legality.
            if (!abilityActivatable(player, id, obj, *card, abilities, ai)) continue;

            auto legal_targets = card->enumerateLegalTargets(
                state_, player, static_cast<int>(ai), id);
            if (ab.targets.count > 0 && legal_targets.empty() &&
                !ab.targets.optional) continue;

            if (ab.needs_activation_time_target) {
                Intent activate;
                activate.type = IntentType::ActivateActionAbility;
                activate.player = player;
                activate.ability_source = id;
                activate.ability_index = static_cast<int>(ai);
                actions.push_back(activate);
                continue;
            }

            if (ab.targets.count == 0) {
                Intent activate;
                activate.type = IntentType::ActivateActionAbility;
                activate.player = player;
                activate.ability_source = id;
                activate.ability_index = static_cast<int>(ai);
                actions.push_back(activate);
            } else {
                for (auto target : legal_targets) {
                    Intent activate;
                    activate.type = IntentType::ActivateActionAbility;
                    activate.player = player;
                    activate.ability_source = id;
                    activate.ability_index = static_cast<int>(ai);
                    activate.targets = {target};
                    actions.push_back(activate);
                }
            }
        }
    }

    return actions;
}

// CR 811.6: a card hidden on an earlier turn has [Reaction] while facedown, so it can be
// played whenever a [Reaction] card can (Open State on its controller's turn, showdowns, all
// Closed States). CR 811.1.d: a spell's targets must be at the battlefield it was hidden at,
// and a spell with no such target can't be played. A card that chooses its target as it is
// played (needsPlayTimeTarget / Pair) gets one intent and chooses at finalization, restricted
// to that battlefield (ChainItem::hidden_at); other spells get one intent per target there.
void GameEngine::appendHiddenPlays(PlayerId player, std::vector<Intent>& actions) const {
    for (auto& bf : state_.battlefields) {
        for (auto card_id : bf.facedown) {
            if (!state_.objectExists(card_id)) continue;
            auto& card = state_.getObject(card_id);
            if (card.controller != player) continue;
            // Must be hidden on a previous turn to gain Reaction
            if (card.hidden_on_turn >= state_.turn.turn_number) continue;
            Intent play;
            play.type = IntentType::PlayReaction;
            play.player = player;
            play.card = card_id;

            const Card* card_obj = (card.card_def_id != kInvalidId && card.isSpell())
                ? card_registry_.get(card.card_def_id) : nullptr;
            if (card_obj && card_obj->getTargetRequirements().count > 0) {
                std::vector<GameObjectId> bf_targets;
                for (auto t : card_obj->enumerateLegalTargets(state_, player))
                    if (state_.choosableFromHiddenAt(t, bf.id)) bf_targets.push_back(t);
                if (bf_targets.empty() && card_obj->hiddenPlayRestrictsTargets()) continue;
                if (card_obj->needsPlayTimeTarget() || card_obj->needsPlayTimeTargetPair()) {
                    actions.push_back(play);
                    continue;
                }
                for (auto t : bf_targets) {
                    Intent targeted = play;
                    targeted.targets = {t};
                    actions.push_back(targeted);
                }
                continue;
            }
            // No-target spell, or a permanent (it goes to the battlefield it was
            // hidden at — or any battlefield for the next unit after Last Caress).
            if (card.isUnit() && state_.player(player).next_unit_any_bf) {
                for (const auto& to : state_.battlefields) {
                    Intent at = play;
                    at.play_location = BattlefieldLocation{to.id};
                    actions.push_back(at);
                }
                continue;
            }
            actions.push_back(play);

        }
    }
}

std::vector<Intent> GameEngine::generateClosedStateActions(
    PlayerId player) const {
    std::vector<Intent> actions;

    // Can always pass priority
    actions.push_back(Intent::passPriority(player));

    const bool locked_out = state_.player(player).cant_play_cards_this_turn;

    // Can only play Reaction spells in Closed State (CR 309.1.a).
    // Brynhir lockout suppresses these.
    if (!locked_out) {
        generateSpellActions(player, /*action_ok=*/false, /*reaction_ok=*/true,
                              actions);
        generateTrashReplayActions(player, /*action_ok=*/false, /*reaction_ok=*/true,
                                    actions);
    }

    // Quick-Draw: play gear with [Quick-Draw] as Reactions targeting a friendly unit
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isGear() || !card.hasKeyword(Keyword::QuickDraw)) continue;
        if (!canAfford(player, card_id)) continue;

        for (auto& [uid, unit] : state_.objects) {
            if (!unit.isUnit() || unit.controller != player) continue;
            if (!unit.location.has_value()) continue;
            Intent qd;
            qd.type = IntentType::PlayReaction;
            qd.player = player;
            qd.card = card_id;
            qd.targets = {uid}; // target unit to auto-attach
            actions.push_back(qd);
        }
    }

    // Ambush: play units with [Ambush] as Reactions to BFs where you have units
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isUnit() || !card.hasKeyword(Keyword::Ambush)) continue;
        if (!canAfford(player, card_id)) continue;

        const Card* amb = card.card_def_id != kInvalidId
            ? card_registry_.get(card.card_def_id) : nullptr;
        const bool enemy_bf_ok = amb && amb->ambushToEnemyBattlefields();
        for (auto& bf : state_.battlefields) {
            if (bf.blocks_unit_play) continue;  // Rockfall Path
            bool here = bf.hasUnitsFrom(player, state_.objects);
            if (!here && enemy_bf_ok)
                here = bf.hasUnitsFrom(opponent(player), state_.objects);
            if (!here) continue;
            Intent ambush;
            ambush.type = IntentType::PlayReaction;
            ambush.player = player;
            ambush.card = card_id;
            ambush.play_location = BattlefieldLocation{bf.id};
            actions.push_back(ambush);
        }
    }

    // Reaction-to-attack plays during Closed State (Rengar, Pouncing).
    for (auto card_id : state_.player(player).hand) {
        if (locked_out) break;
        auto& card = state_.getObject(card_id);
        if (!card.isUnit()) continue;
        if (card.card_def_id == kInvalidId) continue;
        Card* card_obj = card_registry_.get(card.card_def_id);
        if (!card_obj || !card_obj->playableAsReactionToAttack()) continue;
        if (!canAfford(player, card_id)) continue;

        for (auto& bf : state_.battlefields) {
            if (bf.blocks_unit_play) continue;  // Rockfall Path
            if (!bf.combat_in_progress) continue;
            if (!bf.attacker.has_value() || *bf.attacker != player) continue;
            Intent pounce;
            pounce.type = IntentType::PlayReaction;
            pounce.player = player;
            pounce.card = card_id;
            pounce.play_location = BattlefieldLocation{bf.id};
            actions.push_back(pounce);
        }
    }

    // Can play hidden cards (they gain Reaction on next turn after hiding)
    if (!locked_out) appendHiddenPlays(player, actions);

    // CR 376 / 398-406 — activated abilities with [Reaction] timing
    // (e.g. Shadow [752]: "[Action] [1][A], [E]: stun attacker") can be
    // activated during Closed State. Phase 6q+ fix: pre-fix this re-tagged
    // ALL activated abilities as Reaction, leaking non-Reaction abilities
    // (Bounty Hunter [E]) into the closed-state action set. Now filtered
    // via Card::isReactionAbility() — opt-in per card. Tag the emitted
    // intents as ActivateReactionAbility so the dispatcher handles
    // priority correctly.
    if (!locked_out) {
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != player) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            Card* card_obj = card_registry_.get(obj.card_def_id);
            if (!card_obj) continue;
            // Same gates as the main-phase generator (abilityActivatable).
            auto abilities = card_obj->activatedAbilities();
            for (size_t ai = 0; ai < abilities.size(); ++ai) {
                const auto& ab = abilities[ai];
                if (!ab.is_reaction) continue;
                if (!abilityActivatable(player, id, obj, *card_obj, abilities, ai)) continue;

                auto legal_targets = card_obj->enumerateLegalTargets(
                    state_, player, static_cast<int>(ai), id);
                if (ab.targets.count > 0 && legal_targets.empty() &&
                    !ab.targets.optional) continue;

                if (ab.needs_activation_time_target) {
                    Intent a;
                    a.type = IntentType::ActivateReactionAbility;
                    a.player = player;
                    a.ability_source = id;
                    a.ability_index = static_cast<int>(ai);
                    actions.push_back(a);
                    continue;
                }

                if (ab.targets.count == 0) {
                    Intent a;
                    a.type = IntentType::ActivateReactionAbility;
                    a.player = player;
                    a.ability_source = id;
                    a.ability_index = static_cast<int>(ai);
                    actions.push_back(a);
                } else {
                    for (auto t : legal_targets) {
                        Intent a;
                        a.type = IntentType::ActivateReactionAbility;
                        a.player = player;
                        a.ability_source = id;
                        a.ability_index = static_cast<int>(ai);
                        a.targets = {t};
                        actions.push_back(a);
                    }
                }
            }
        }
    }

    return actions;
}

void GameEngine::generateSpellActions(PlayerId player, bool action_ok,
                                       bool reaction_ok,
                                       std::vector<Intent>& actions) const {
    auto& ps = state_.player(player);
    // Lilting Lullaby / cards-lockout: zero spell-play actions while the
    // controller is under either lockout flag this turn.
    if (ps.cant_play_cards_this_turn || ps.cant_play_spells_this_turn) return;
    const int option_disc = spellCostOptionDiscount(player);   // Starry-Eyed Songstress
    for (const auto& [card_id, src] : playableCards(player)) {
        PlaySourceStamp stamp{actions, actions.size(), src};
        auto& card = state_.getObject(card_id);
        if (!card.isSpell()) continue;
        if (!canAfford(player, card_id) &&
            (option_disc == 0 || !canAfford(player, card_id, 0, option_disc))) continue;


        // Check timing keywords
        bool has_action = card.keywords.has(Keyword::Action);
        bool has_reaction = card.keywords.has(Keyword::Reaction);

        // Reaction grants all Action permissions too (CR 813.1.b)
        if (has_reaction) has_action = true;

        // In Neutral Open, any spell can be played (no timing restriction)
        // In Showdown, need Action or Reaction
        // In Closed State, need Reaction only
        bool allowed = false;
        if (state_.turn.isNeutralOpen()) {
            allowed = true; // any spell
        } else if (state_.turn.isShowdownOpen()) {
            allowed = has_action || has_reaction;
        } else if (state_.turn.isClosedState()) {
            allowed = has_reaction;
            if (!reaction_ok) allowed = false;
        } else {
            if (action_ok && has_action) allowed = true;
            if (reaction_ok && has_reaction) allowed = true;
        }

        if (!allowed) continue;

        // Get legal targets for this spell via Card object
        Card* spell_card = card_registry_.get(card.card_def_id);

        // Universal legality gate (CR 700.x). Default impl preserves the
        // historical "needs target but none exist" behavior; counter spells
        // with no chain target override this so they're not offered as legal
        // moves when they'd silently no-op.
        if (spell_card && !spell_card->hasLegalTargets(state_, player)) {
            continue;
        }

        auto legal_targets = spell_card
            ? spell_card->enumerateLegalTargets(state_, player)
            : std::vector<GameObjectId>{};
        auto req = spell_card
            ? spell_card->getTargetRequirements()
            : TargetRequirements{};

        // Determine intent type based on context
        IntentType intent_type = IntentType::PlayCard;
        if (state_.turn.isShowdownOpen()) {
            intent_type = IntentType::PlayActionCard;
        } else if (state_.turn.isClosedState()) {
            intent_type = IntentType::PlayReaction;
        }

        // Repeat (CR 820) is now an in-play decision (see
        // executePlaySpell). The action generator emits one Play intent
        // per spell; after base cost is paid the engine polls the agent
        // yes/no for each affordable tranche. Pre-fix this loop emitted
        // one Play per repeat-count up to 6 — which produced 4+
        // visually-identical "Play Hard Bargain" entries in the legal
        // list and forced the agent to commit to N before knowing what
        // the counterspell would resolve against. CR 820 is the same
        // shape as Accelerate's "you may pay additional" — a yes/no at
        // cost time, not a pre-encoded vocab slot.
        auto emit = [&](Intent play, int /*R*/) {
            actions.push_back(play);
        };
        constexpr int kSingleEmit = 0;  // sentinel arg for emit() readability

        // Phase 6q — target decoupling. When the card opts in via
        // needsPlayTimeTarget() or needsPlayTimeTargetPair(), emit
        // ONE Play intent per card with NO targets. The card's
        // onResolve then uses Card::pickTarget / pickTargetPair to
        // publish follow-up MakeChoice intents keyed by target
        // card_def_id (Phase 5g int-coded MakeChoice slots). This
        // gives the AlphaZero policy head distinct vocab slots per
        // target choice instead of collapsing all (card, target)
        // variants (or the (friendly × enemy) Cartesian product for
        // dual-target spells like Challenge) into one Play slot.
        // hasLegalTargets() above already guarantees ≥1 legal target
        // exists at play time; pickTarget re-enumerates at resolve
        // time for CR-correctness against chain-driven state shifts.
        if (spell_card && req.count > 0 &&
            (spell_card->needsPlayTimeTarget() ||
             spell_card->needsPlayTimeTargetPair())) {
            Intent play;
            play.type = intent_type;
            play.player = player;
            play.card = card_id;
            // targets intentionally empty — agent picks via
            // pickTarget / pickTargetPair at resolve time.
            emit(play, kSingleEmit);
            continue;
        }

        if (req.count == 0) {
            // No targeting needed
            Intent play;
            play.type = intent_type;
            play.player = player;
            play.card = card_id;
            emit(play, kSingleEmit);
        } else if (req.optional && legal_targets.empty()) {
            // Optional targeting with no targets — play without targets
            Intent play;
            play.type = intent_type;
            play.player = player;
            play.card = card_id;
            emit(play, kSingleEmit);
        } else if (req.count == 2) {
            // Dual targeting (e.g., Challenge: friendly + enemy)
            // Separate targets into friendly and enemy
            std::vector<GameObjectId> friendly_targets, enemy_targets;
            for (auto tid : legal_targets) {
                auto& t = state_.getObject(tid);
                if (t.controller == player) friendly_targets.push_back(tid);
                else enemy_targets.push_back(tid);
            }
            // Generate all valid pairs
            for (auto ft : friendly_targets) {
                for (auto et : enemy_targets) {
                    Intent play;
                    play.type = intent_type;
                    play.player = player;
                    play.card = card_id;
                    play.targets = {ft, et};
                    emit(play, kSingleEmit);
                }
            }
        } else {
            // Generate one intent per legal target
            for (auto target : legal_targets) {
                Intent play;
                play.type = intent_type;
                play.player = player;
                play.card = card_id;
                play.targets = {target};
                emit(play, kSingleEmit);
            }
        }
    }
}

void GameEngine::generateTrashReplayActions(PlayerId player, bool action_ok,
                                             bool reaction_ok,
                                             std::vector<Intent>& actions) const {
    auto& ps = state_.player(player);
    if (ps.trash_replay_grants.empty()) return;
    if (ps.cant_play_cards_this_turn || ps.cant_play_spells_this_turn) return;

    for (const auto& grant : ps.trash_replay_grants) {
        if (grant.card == kInvalidId) continue;
        if (!state_.objectExists(grant.card)) continue;
        auto& card = state_.getObject(grant.card);
        if (card.zone != ZoneType::Trash) continue;   // grant valid only in trash
        if (!card.isSpell()) continue;

        // Timing gate — identical to generateSpellActions (CR 309/806/813).
        bool has_action = card.keywords.has(Keyword::Action);
        bool has_reaction = card.keywords.has(Keyword::Reaction);
        if (has_reaction) has_action = true;
        bool allowed = false;
        if (state_.turn.isNeutralOpen()) {
            allowed = true;
        } else if (state_.turn.isShowdownOpen()) {
            allowed = has_action || has_reaction;
        } else if (state_.turn.isClosedState()) {
            allowed = has_reaction && reaction_ok;
        } else {
            if (action_ok && has_action) allowed = true;
            if (reaction_ok && has_reaction) allowed = true;
        }
        if (!allowed) continue;

        // Affordability against the OVERRIDE cost, not the printed cost. For
        // an [A] grant, any single domain that's payable suffices.
        const SpendPurpose grant_purpose = state_.objectExists(grant.card)
            ? state_.purposePlaying(state_.getObject(grant.card).card_type)
            : state_.purposeOther();
        bool affordable = false;
        if (grant.any_domain) {
            for (int di = 0; di < static_cast<int>(Domain::Count); ++di) {
                if (canPayAdditionalCost(player, grant.energy, grant.power,
                                          static_cast<Domain>(di), grant_purpose)) {
                    affordable = true;
                    break;
                }
            }
        } else {
            affordable = canPayAdditionalCost(player, grant.energy, grant.power,
                                               grant.power_domain, grant_purpose);
        }
        if (!affordable) continue;

        Card* spell_card = card_registry_.get(card.card_def_id);
        if (spell_card && !spell_card->hasLegalTargets(state_, player)) continue;
        auto legal_targets = spell_card
            ? spell_card->enumerateLegalTargets(state_, player)
            : std::vector<GameObjectId>{};
        auto req = spell_card ? spell_card->getTargetRequirements()
                              : TargetRequirements{};

        IntentType intent_type = IntentType::PlayCard;
        if (state_.turn.isShowdownOpen()) intent_type = IntentType::PlayActionCard;
        else if (state_.turn.isClosedState()) intent_type = IntentType::PlayReaction;

        auto make = [&](std::vector<GameObjectId> tgts) {
            Intent play;
            play.type = intent_type;
            play.player = player;
            play.card = grant.card;
            play.play_source = Intent::PlaySource::Trash;
            play.targets = std::move(tgts);
            actions.push_back(play);
        };

        if (spell_card && req.count > 0 &&
            (spell_card->needsPlayTimeTarget() ||
             spell_card->needsPlayTimeTargetPair())) {
            make({});                                  // agent picks at resolve
        } else if (req.count == 0 || (req.optional && legal_targets.empty())) {
            make({});
        } else if (req.count == 2) {
            std::vector<GameObjectId> friendly, enemy;
            for (auto tid : legal_targets) {
                if (state_.getObject(tid).controller == player) friendly.push_back(tid);
                else enemy.push_back(tid);
            }
            for (auto ft : friendly)
                for (auto et : enemy) make({ft, et});
        } else {
            for (auto target : legal_targets) make({target});
        }
    }
}

bool GameEngine::abilityActivatable(PlayerId player, GameObjectId id, const GameObject& obj,
                                    const Card& card_ref,
                                    const std::vector<ActivatedAbility>& abilities,
                                    size_t ai) const {
    const Card* card = &card_ref;
    // Per-card "Use only if ..." gate (Emperor of the Sands etc.).
    if (!card->canActivateAbility(state_, player)) return false;
    const auto& ab = abilities[ai];
    const auto& act_cost = ab.cost;

    // Per-ability gate ([Empower]: only if not Empowered; "Disempower
    // this" costs need an Empowered status).
    if (!card->canActivateAbilityAt(state_, player, id, (int)ai)) return false;

    // Check activation cost: must be ready if exhaust required
    if (act_cost.exhaust && obj.is_exhausted) return false;
    {
        // Risen Altar's part of the reduction may come off Energy or Power.
        int red = activationEnergyReduction(*card, player, id, (int)ai);
        const int altar = ab.is_empower ? std::min(red, obj.empower_cost_reduction) : 0;
        red -= altar;
        const auto purpose = state_.purposeAbilityOf(obj.card_type);
        auto payable = [&](int energy, int power) {
            ActivationCost c = act_cost;
            c.power = power;
            if (energy > 0 && availableEnergy(player, purpose) < energy) return false;
            return power == 0 || canPayActivationResources(player, energy, c, purpose);
        };
        const int e0 = std::max(0, act_cost.energy - red);
        if (!payable(std::max(0, e0 - altar), act_cost.power) &&
            !(altar > 0 && payable(e0, std::max(0, act_cost.power - altar))))
            return false;
    }
    if (act_cost.discard && act_cost.discard_count > 0) {
        int matching = 0;
        for (auto hid : state_.player(player).hand) {
            if (act_cost.discard_type_filter == 0 ||
                static_cast<int>(state_.getObject(hid).card_type) + 1 ==
                    act_cost.discard_type_filter) ++matching;
        }
        if (matching < act_cost.discard_count) return false;
    }
    if (act_cost.xp_cost > 0 &&
        state_.player(player).xp < act_cost.xp_cost) return false;

    // Universal legality gate (CR 700.x). For multi-ability cards
    // the per-ability target requirements live on the descriptor;
    // ALWAYS consult ab.targets first. Only fall back to the
    // legacy Card::hasLegalTargets path for single-ability cards
    // that don't declare per-ability targets — e.g. counter
    // spells that gate on chain-item presence via their own
    // hasLegalTargets override. This fixes a bug where single-
    // ability cards with .targets.count>0 were being offered
    // even when no legal board targets existed (Bounty Hunter
    // at Turn 0 with no units on the board).
    if (ab.targets.count > 0 && !ab.targets.optional) {
        auto legal = card->enumerateLegalTargets(state_, player,
                                                  static_cast<int>(ai), id);
        if (legal.empty()) return false;
    } else if (abilities.size() == 1 &&
                !card->hasLegalTargets(state_, player)) {
        return false;
    }
    return true;
}

void GameEngine::generateActivateAbilityActions(PlayerId player,
                                                  std::vector<Intent>& actions) const {
    // Phase 6r — multi-ability per Card. Loop over each ability descriptor
    // on the card; each gets its own affordability check and target loop.
    // Single-ability cards use the default Card::activatedAbilities() impl
    // which wraps the legacy single-ability virtuals into a one-element
    // vector — semantics unchanged for them.
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != player) continue;
        if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;

        Card* card = card_registry_.get(obj.card_def_id);
        if (!card) continue;
        // Per-card "Use only if …" gate (Emperor of the Sands etc.).
        if (!card->canActivateAbility(state_, player)) continue;
        auto abilities = card->activatedAbilities();
        // Don't skip when the object carries no OWN abilities but has been
        // granted some by an aura (Forge/Gardens/Heimerdinger).
        if (abilities.empty() && obj.granted_abilities.empty()) continue;

        for (size_t ai = 0; ai < abilities.size(); ++ai) {
            const auto& ab = abilities[ai];
            if (ab.granted_only) continue;   // only the objects it is granted to use it
            if (!abilityActivatable(player, id, obj, *card, abilities, ai)) continue;

            // Phase 6q+ — if this ability defers target selection to
            // resolve-time via pickTarget (action-vocab collision fix),
            // emit ONE intent with empty targets regardless of count.
            if (ab.needs_activation_time_target) {
                Intent activate;
                activate.type = IntentType::ActivateAbility;
                activate.player = player;
                activate.ability_source = id;
                activate.ability_index = static_cast<int>(ai);
                actions.push_back(activate);
                continue;
            }

            if (ab.targets.count == 0) {
                Intent activate;
                activate.type = IntentType::ActivateAbility;
                activate.player = player;
                activate.ability_source = id;
                activate.ability_index = static_cast<int>(ai);
                actions.push_back(activate);
            } else {
                auto legal_targets = card->enumerateLegalTargets(
                    state_, player, static_cast<int>(ai), id);
                for (auto target : legal_targets) {
                    Intent activate;
                    activate.type = IntentType::ActivateAbility;
                    activate.player = player;
                    activate.ability_source = id;
                    activate.ability_index = static_cast<int>(ai);
                    activate.targets = {target};
                    actions.push_back(activate);
                }
            }
        }

        // Aura-granted activated abilities (Forge/Gardens/Heimerdinger): each ref
        // names a granting card + ability index the bearer (this object) may now
        // use. Cost is read from the granter's descriptor and paid by the bearer.
        for (const auto& ref : obj.granted_abilities) {
            Card* gcard = card_registry_.get(ref.source_def_id);
            if (!gcard) continue;
            auto gabs = gcard->activatedAbilities();
            if (ref.ability_index < 0 ||
                ref.ability_index >= static_cast<int>(gabs.size())) continue;
            const auto& gcost = gabs[ref.ability_index].cost;
            if (ref.exhaust_cost_only && !gcost.exhaust) continue;
            if (gcost.exhaust && obj.is_exhausted) continue;           // bearer must be ready
            if (gcost.energy > 0 && availableEnergy(player, state_.purposeAbilityOf(obj.card_type)) < gcost.energy) continue;
            if (gcost.power > 0 && !canPayActivationResources(player, gcost.energy, gcost,
                                                                     state_.purposeAbilityOf(obj.card_type))) continue;
            if (gcost.xp_cost > 0 && state_.player(player).xp < gcost.xp_cost) continue;
            Intent activate;
            activate.type = IntentType::ActivateAbility;
            activate.player = player;
            activate.ability_source = id;
            activate.ability_index = ref.ability_index;
            activate.granted_ability_def = ref.source_def_id;
            actions.push_back(activate);
        }
    }

    // Battlefield abilities. A battlefield's card object has no location, so
    // the loop above never sees it: its controller may use its abilities, and
    // so may any player when the card says so (Black Market).
    for (const auto& bf : state_.battlefields) {
        if (!state_.objectExists(bf.card_object_id)) continue;
        const auto& obj = state_.getObject(bf.card_object_id);
        if (obj.location.has_value() || obj.card_def_id == kInvalidId) continue;
        Card* card = card_registry_.get(obj.card_def_id);
        if (!card) continue;
        const bool mine = bf.controller.has_value() && *bf.controller == player;
        if (!mine && !card->activatableByAnyPlayer()) continue;
        auto abilities = card->activatedAbilities();
        for (size_t ai = 0; ai < abilities.size(); ++ai) {
            const auto& ab = abilities[ai];
            if (ab.granted_only || ab.targets.count > 0) continue;
            if (!abilityActivatable(player, bf.card_object_id, obj, *card, abilities, ai)) continue;
            Intent activate;
            activate.type = IntentType::ActivateAbility;
            activate.player = player;
            activate.ability_source = bf.card_object_id;
            activate.ability_index = static_cast<int>(ai);
            actions.push_back(activate);
        }
    }
}

std::vector<Intent> GameEngine::generateCombatDamageActions(
    PlayerId player) const {
    std::vector<Intent> actions;
    // Phase 1: simplified — assign all damage to one unit at a time,
    // obeying lethal-before-next rule (CR 460.2.c.3)
    // For now, generate a single "spread evenly" assignment
    // TODO: Full damage assignment enumeration

    // Find the battlefield with active combat
    for (auto& bf : state_.battlefields) {
        if (!bf.combat_in_progress) continue;

        PlayerId enemy = opponent(player);
        auto enemy_units = state_.unitsAt(
            BattlefieldLocation{bf.id}, enemy);
        auto my_units = state_.unitsAt(
            BattlefieldLocation{bf.id}, player);

        if (enemy_units.empty() || my_units.empty()) continue;

        // Sum my might
        int total_might = 0;
        for (auto uid : my_units) {
            total_might += state_.getObject(uid).current_might;
        }

        // Generate one assignment: kill units in order until damage runs out
        std::vector<DamageAssignment> assignments;
        int remaining = total_might;
        for (auto target_id : enemy_units) {
            if (remaining <= 0) break;
            auto& target = state_.getObject(target_id);
            int lethal = target.current_might - target.damage_marked;
            if (lethal <= 0) continue;
            int assigned = std::min(remaining, lethal);
            assignments.push_back({target_id, assigned});
            remaining -= assigned;
        }
        // Dump remaining damage on last unit
        if (remaining > 0 && !assignments.empty()) {
            assignments.back().damage += remaining;
        }

        actions.push_back(Intent::assignCombatDamage(player, assignments));
        break;
    }

    return actions;
}

} // namespace riftbound
