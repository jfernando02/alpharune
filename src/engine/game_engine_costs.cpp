#include <cstdio>
#include <cstdlib>
#include "game_engine.h"
#include "engine/game_engine_internal.h"
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
// Cost payment
// ═══════════════════════════════════════════════════════════════════════════════

int GameEngine::availableEnergy(PlayerId player, SpendPurpose purpose) const {
    // Energy comes from exhausting ready runes (CR 163.2.a: [E]: Add [1])
    // Each ready rune in base can be exhausted for 1 Energy
    int count = 0;
    auto base_loc = BaseLocation{player};
    for (auto& [id, obj] : state_.objects) {
        if (obj.isRune() && obj.controller == player && !obj.is_exhausted &&
            obj.location.has_value() && *obj.location == LocationId{base_loc}) {
            count++;
        }
    }
    // Also count Energy already in the pool
    purpose.in_showdown = state_.inShowdown();
    count += state_.player(player).rune_pool.energyFor(purpose);
    return count;
}

int GameEngine::availablePower(PlayerId player, Domain domain) const {
    // Power comes from recycling a rune of matching domain (CR 163.2.b)
    // Each rune in base (exhausted or ready) of matching domain can be recycled for 1 Power
    int count = 0;
    auto base_loc = BaseLocation{player};
    for (auto& [id, obj] : state_.objects) {
        if (obj.isRune() && obj.controller == player &&
            obj.location.has_value() && *obj.location == LocationId{base_loc}) {
            for (auto d : obj.domains) {
                if (d == domain) { count++; break; }
            }
        }
    }
    // Also count matching Power already in pool
    count += state_.player(player).rune_pool.power[static_cast<int>(domain)];
    count += state_.player(player).rune_pool.universal_power;
    return count;
}

int GameEngine::availableAnyPower(PlayerId player) const {
    // Any rune in base can be recycled for 1 Power of its domain
    int count = 0;
    auto base_loc = BaseLocation{player};
    for (auto& [id, obj] : state_.objects) {
        if (obj.isRune() && obj.controller == player &&
            obj.location.has_value() && *obj.location == LocationId{base_loc}) {
            count++;
        }
    }
    return count;
}

// ─── Repeat (CR 820) ────────────────────────────────────────────────────────
//
// Parses the FIRST `[Repeat] [...]` token in ability_text. See header doc.
//
// Recognized forms:
//   [Repeat] [N]       → N energy
//   [Repeat] [N][D]    → N energy + 1 domain power (D ∈ A/B/C/F/O/M/P/R/U/V)
//   [Repeat] [D]       → 1 domain power
//   [Repeat] [A]       → 1 universal power
//
// Cards with multi-mode Repeat costs (Curtain Call: "[Repeat] [1]/[A]/[1][A]")
// are handled by the card's own onResolve modal logic, not by this parse.
GameEngine::RepeatCost GameEngine::parseRepeatCost(const std::string& text) {
    RepeatCost rc;
    auto pos = text.find("[Repeat]");
    if (pos == std::string::npos) return rc;
    pos += std::string("[Repeat]").size();
    // Skip whitespace
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;

    // First token: either [N] (digits) or [D] (single letter)
    if (pos >= text.size() || text[pos] != '[') return rc;
    auto end = text.find(']', pos);
    if (end == std::string::npos) return rc;
    std::string tok = text.substr(pos + 1, end - pos - 1);

    // CR 134.2 canonical domain-letter mapping (also in types.h:62-67):
    //   R = Fury (red), G = Calm (green), B = Mind (blue),
    //   O = Body (orange), P = Chaos (purple), Y = Order (yellow).
    //
    // The pre-2026-05-19 mapping had 4 wrong entries (B→Body, O→Order,
    // P→Mind, R→Calm) plus speculative entries for C/F/M/V/U that do
    // not appear in any printed Riftbound card text. Cross-checked
    // against cards/card_index.json — every [Repeat] [N][D] cost token
    // in the registry uses one of {R, G, B, O, P, Y} (or [A] for
    // universal). Symptoms of the bug: Piercing Light (Fury) `[R]`
    // tranches were paid from the Calm rune pool; Rocket Barrage /
    // Bellows Breath (Mind) `[B]` from Body; Called Shot / Syndra
    // (Chaos) `[P]` from Mind. Affordability + payment both wrong.
    // Phase 6q+ engine-audit CRITICAL #4.
    auto domainFromLetter = [](char c) -> Domain {
        switch (c) {
            case 'R': return Domain::Fury;
            case 'G': return Domain::Calm;
            case 'B': return Domain::Mind;
            case 'O': return Domain::Body;
            case 'P': return Domain::Chaos;
            case 'Y': return Domain::Order;
            default:  return Domain::Count;
        }
    };

    auto allDigits = [](const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    };

    if (allDigits(tok)) {
        rc.energy = std::stoi(tok);
        rc.valid = true;
        // Look for an optional trailing [D] domain power token.
        pos = end + 1;
        if (pos < text.size() && text[pos] == '[') {
            auto end2 = text.find(']', pos);
            if (end2 != std::string::npos) {
                std::string tok2 = text.substr(pos + 1, end2 - pos - 1);
                if (tok2 == "A") {
                    rc.power = 1;
                    rc.power_domain = Domain::Count;  // universal
                } else if (tok2.size() == 1) {
                    Domain d = domainFromLetter(tok2[0]);
                    if (d != Domain::Count) {
                        rc.power = 1;
                        rc.power_domain = d;
                    }
                }
            }
        }
    } else if (tok == "A") {
        rc.power = 1;
        rc.power_domain = Domain::Count;
        rc.valid = true;
    } else if (tok.size() == 1) {
        Domain d = domainFromLetter(tok[0]);
        if (d != Domain::Count) {
            rc.power = 1;
            rc.power_domain = d;
            rc.valid = true;
        }
    }
    return rc;
}

// Pay one additional Repeat tranche (R is the per-tranche cost). Iterates
// the same primitives canAfford / payCardCost use, but for ONE extra
// (energy, domain_power) charge — called by executePlaySpell after the
// base cost has already been paid.
bool GameEngine::payRepeatCost(PlayerId player, const RepeatCost& cost,
                               SpendPurpose purpose) {
    if (!cost.valid) return true;
    // Floating resources first, then exhaust runes for Energy and recycle runes
    // (ready or exhausted, CR 164.2.b) to the rune deck for Power.
    return payAdditionalCost(player, cost.energy, cost.power, cost.power_domain, purpose);
}

void GameEngine::maybePayOptionalAdditionalCost(PlayerId player, GameObjectId card_id) {
    if (!state_.objectExists(card_id)) return;
    auto& card = state_.getObject(card_id);
    if (card.card_def_id == kInvalidId) return;
    const Card* cdef = card_registry_.get(card.card_def_id);
    if (!cdef) return;
    auto cost = cdef->optionalAdditionalCost();
    if (!cost.valid) return;
    // Discard-as-additional-cost-to-reduce (Brazen Buccaneer) is decided BEFORE
    // payment in executePlayCard (the discount must precede the base cost), so
    // it is NOT handled here.
    if ((cost.discard_cards > 0 || cost.spend_xp > 0) && cost.reduce_energy > 0) return;
    // Kill / return-a-gear costs are decided in executePlayCard too.
    if (cost.kill_friendly_unit || cost.kill_friendly_gear || cost.return_friendly_gear ||
        cost.kill_any_number) return;

    // Pure "you may discard N as an additional cost" (Zed, From the Shadows;
    // Ruthless Strike): offered whenever the hand can cover it.
    if (cost.discard_cards > 0) {
        if (static_cast<int>(state_.player(player).hand.size()) < cost.discard_cards) return;
        Intent no;  no.type  = IntentType::MakeChoice; no.player  = player; no.chosen_value  = 0;
        Intent yes; yes.type = IntentType::MakeChoice; yes.player = player; yes.chosen_value = 1;
        std::vector<Intent> dopts = {no, yes};
        state_.decision_index++;
        events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                         toString(player) + "): discard " +
                         std::to_string(cost.discard_cards) + " as additional cost for " +
                         card.name + "? [decline|pay] [2 options]");
        auto dchosen = getAgent(player).selectAction(state_, dopts);
        recordAppliedIntent(dchosen);
        if (on_decision) on_decision(state_, dopts, dchosen);
        if (dchosen.chosen_value.value_or(0) == 1) {
            effect_executor_->discardCards(player, cost.discard_cards);
            if (cost.paid_flag && cost.paid_flag[0] != '\0')
                card.card_counters[cost.paid_flag] = 1;
        }
        return;
    }

    // Pick a payable domain for the power portion (specific, any, or none).
    // The cost is part of playing the card.
    const SpendPurpose purpose = state_.purposePlaying(card.card_type);
    std::optional<Domain> dom;
    if (cost.power == 0) {
        if (canPayAdditionalCost(player, cost.energy, 0, Domain::Fury, purpose)) dom = Domain::Fury;
    } else if (cost.any_domain) {
        for (int di = 0; di < static_cast<int>(Domain::Count); ++di) {
            if (canPayAdditionalCost(player, cost.energy, cost.power, static_cast<Domain>(di),
                                     purpose)) {
                dom = static_cast<Domain>(di);
                break;
            }
        }
    } else if (canPayAdditionalCost(player, cost.energy, cost.power, cost.power_domain,
                                    purpose)) {
        dom = cost.power_domain;
    }
    if (!dom.has_value()) return;  // can't afford -> not offered (CR: choice only when payable)

    // Optional ("you may") -> yes/no agent decision. Mirrors the Accelerate flow.
    Intent decline; decline.type = IntentType::MakeChoice; decline.player = player; decline.chosen_value = 0;
    Intent accept;  accept.type  = IntentType::MakeChoice; accept.player  = player; accept.chosen_value  = 1;
    std::vector<Intent> opts = {decline, accept};
    state_.decision_index++;
    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                     toString(player) + "): pay additional cost for " + card.name +
                     "? [decline|pay] [2 options]");
    auto chosen = getAgent(player).selectAction(state_, opts);
    recordAppliedIntent(chosen);
    if (on_decision) on_decision(state_, opts, chosen);

    if (chosen.chosen_value.value_or(0) == 1) {
        payAdditionalCost(player, cost.energy, cost.power, *dom, purpose);
        if (cost.paid_flag && cost.paid_flag[0] != '\0') {
            card.card_counters[cost.paid_flag] = 1;
        }
        events_.logTrace("ADDITIONAL_COST: " + card.name + " paid optional additional cost");
    } else {
        events_.logTrace("ADDITIONAL_COST: " + card.name + " declined additional cost");
    }
}

// Net change to a card's POWER cost from continuous effects and cost modifiers
// (Helm of Suppression's "[A] more", Applied Researchers' "[A] less", Astral
// Heron's "[A][A] less"). Shared by canAfford and beginCostPayment so the two
// always agree.
// Extra POWER OF ANY DOMAIN ([A]) owed on top of a card's own cost: "cost [A]
// more" effects, which are not tied to the card's domains (Helm of
// Suppression taxes the opponent's spells; Mystic Vortex taxes [Reaction]
// cards during a showdown at its battlefield). Paid after the base cost.
static int extraAnyPowerCost(const GameState& state, PlayerId player, const GameObject& card) {
    int extra = 0;
    if (card.isSpell()) extra += state.player(player).spell_tax_power;
    // "(Hidden cards have [Reaction].)" — and [Ambush] is Reaction while played.
    const bool has_reaction = card.keywords.has(Keyword::Reaction) || card.is_hidden ||
                              card.keywords.has(Keyword::Ambush);
    if (has_reaction) {
        int tax = 0;
        for (const auto& bf : state.battlefields)
            if ((bf.showdown_in_progress || bf.combat_in_progress) && bf.reaction_tax_power > tax)
                tax = bf.reaction_tax_power;
        extra += tax;
    }
    return std::max(0, extra);
}

static int powerCostDelta(const GameState& state, PlayerId player, const GameObject& card,
                          const Card* card_class) {
    const auto& ps = state.player(player);
    int delta = 0;
    if (card.isSpell()) delta -= ps.spell_discount_power;
    delta -= ps.transient_power_discount;
    for (const auto& mod : ps.cost_modifiers) {
        if (mod.power_reduction == 0 || mod.affects_enemy_only) continue;
        if (mod.next_spell_only && !card.isSpell()) continue;
        if (mod.next_unit_only && !card.isUnit()) continue;
        if (!mod.appliesToCard(card)) continue;
        if (mod.first_gear_per_turn && ps.gears_played_this_turn > 0) continue;
        if (mod.once_per_turn_optional && onceDiscountSpent(state, mod)) continue;
        delta -= mod.power_reduction;
    }
    if (card_class) delta -= card_class->selfPowerCostReduction(state, player);
    return delta;
}

int GameEngine::playHereDiscount(PlayerId player, BattlefieldId bf_id,
                                 const GameObject& card) const {
    for (const auto& bf : state_.battlefields) {
        if (bf.id != bf_id || !state_.objectExists(bf.card_object_id)) continue;
        const auto& bobj = state_.getObject(bf.card_object_id);
        if (bobj.card_def_id == kInvalidId) return 0;
        const Card* c = card_registry_.get(bobj.card_def_id);
        return c ? c->playHereDiscount(state_, bf_id, player, card) : 0;
    }
    return 0;
}

namespace {
// Ready friendly units `player` controls on the board (cost fodder).
std::vector<GameObjectId> readyFriendlyUnits(const GameState& s, PlayerId player) {
    std::vector<GameObjectId> out;
    for (const auto& [id, o] : s.objects)
        if (o.isUnit() && o.controller == player && o.location.has_value() && !o.is_exhausted)
            out.push_back(id);
    return out;
}
}  // namespace

std::pair<int, int> GameEngine::additionalCostDiscount(const Card::OptionalAdditionalCost& oac,
                                                      GameObjectId fodder) const {
    int energy = oac.reduce_energy, power = 0;
    if (oac.reduce_by_killed_cost && state_.objectExists(fodder)) {
        const auto& f = state_.getObject(fodder);
        if (f.card_def_id != kInvalidId && f.super_type != SuperType::Token) {
            energy += card_db_.get(f.card_def_id).energy_cost;
            power += card_db_.get(f.card_def_id).power_cost;
        }
    }
    return {energy, power};
}

bool GameEngine::affordableWithAdditionalCost(PlayerId player, GameObjectId card_obj) const {
    if (!state_.objectExists(card_obj)) return false;
    const auto& card = state_.getObject(card_obj);
    const Card* pc = card.card_def_id != kInvalidId ? card_registry_.get(card.card_def_id) : nullptr;
    if (!pc) return false;
    const auto oac = pc->optionalAdditionalCost();
    if (!oac.valid) return false;
    if ((oac.discard_cards > 0 || oac.spend_xp > 0) && oac.reduce_energy > 0) {
        const int others = static_cast<int>(state_.player(player).hand.size()) - 1;
        return others >= oac.discard_cards && state_.player(player).xp >= oac.spend_xp &&
               canAfford(player, card_obj, 0, oac.reduce_energy);
    }
    if (oac.kill_any_number) {
        int units = 0;
        for (const auto& [oid, o] : state_.objects)
            if (oid != card_obj && o.controller == player && o.isUnit() && o.location.has_value() &&
                pc->additionalCostAccepts(state_, o)) ++units;
        return canAfford(player, card_obj, 0, 0, units * oac.power_reduction_per_kill);
    }
    if (!oac.kill_friendly_unit && !oac.kill_friendly_gear && !oac.return_friendly_gear) return false;
    for (const auto& [oid, o] : state_.objects) {
        if (oid == card_obj || o.controller != player || !o.location.has_value()) continue;
        const bool fits = (oac.kill_friendly_unit && o.isUnit()) ||
                          ((oac.kill_friendly_gear || oac.return_friendly_gear) && o.isGear());
        if (!fits || !pc->additionalCostAccepts(state_, o)) continue;
        const auto d = additionalCostDiscount(oac, oid);
        if ((d.first > 0 || d.second > 0) && canAfford(player, card_obj, 0, d.first, d.second))
            return true;
    }
    return false;
}

int GameEngine::spellCostOptionDiscount(PlayerId player) const {
    int best = 0;
    const int ready_units = static_cast<int>(readyFriendlyUnits(state_, player).size());
    for (const auto& [id, o] : state_.objects) {
        if (o.controller != player || o.is_exhausted || o.card_def_id == kInvalidId) continue;
        if (!o.location.has_value() && o.zone != ZoneType::LegendZone) continue;
        const Card* c = card_registry_.get(o.card_def_id);
        if (!c) continue;
        const auto opt = c->spellCostOption();
        if (!opt.valid) continue;
        // The source itself is exhausted too; a unit source can't double as fodder.
        const int fodder = ready_units - (o.isUnit() ? 1 : 0);
        if (fodder >= opt.exhaust_friendly_units) best = std::max(best, opt.reduce_energy);
    }
    return best;
}

int GameEngine::maybePaySpellCostOption(PlayerId player, GameObjectId spell, bool required) {
    for (auto& [id, o] : state_.objects) {
        if (o.controller != player || o.is_exhausted || o.card_def_id == kInvalidId) continue;
        if (!o.location.has_value() && o.zone != ZoneType::LegendZone) continue;
        const Card* c = card_registry_.get(o.card_def_id);
        if (!c) continue;
        const auto opt = c->spellCostOption();
        if (!opt.valid) continue;
        auto fodder = readyFriendlyUnits(state_, player);
        fodder.erase(std::remove(fodder.begin(), fodder.end(), id), fodder.end());
        if (static_cast<int>(fodder.size()) < opt.exhaust_friendly_units) continue;
        const GameObjectId source = id;
        Intent decline; decline.type = IntentType::MakeChoice; decline.player = player;
        decline.chosen_value = 0;
        Intent accept = decline; accept.chosen_value = 1;
        std::vector<Intent> opts = {decline, accept};
        state_.decision_index++;
        events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                         toString(player) + "): exhaust " + o.name + " and " +
                         std::to_string(opt.exhaust_friendly_units) +
                         " friendly units to reduce the cost of " +
                         state_.getObject(spell).name + "?");
        if (!required) {   // when the spell can't be paid for otherwise, it is taken
            auto chosen = getAgent(player).selectAction(state_, opts);
            recordAppliedIntent(chosen);
            if (on_decision) on_decision(state_, opts, chosen);
            if (chosen.chosen_value.value_or(0) != 1) return 0;
        }
        effect_executor_->exhaustObject(source);
        for (int k = 0; k < opt.exhaust_friendly_units; ++k) {
            auto pick = effect_executor_->chooseObjectNow(player, fodder);
            if (pick == kInvalidId) break;
            effect_executor_->exhaustObject(pick);
            fodder.erase(std::remove(fodder.begin(), fodder.end(), pick), fodder.end());
        }
        events_.logTrace("ADDITIONAL_COST: " + state_.getObject(source).name +
                         " reduces the cost of " + state_.getObject(spell).name +
                         " by " + std::to_string(opt.reduce_energy));
        return opt.reduce_energy;
    }
    return 0;
}

bool GameEngine::canAfford(PlayerId player, GameObjectId card_obj, int extra_any_power,
                           int energy_discount, int power_discount) const {

    // Play-legality gates that sit in front of every play generator.
    if (state_.objectExists(card_obj)) {
        const auto& gate_obj = state_.getObject(card_obj);
        if (gate_obj.card_def_id != kInvalidId) {
            if (const Card* gate_card = card_registry_.get(gate_obj.card_def_id))
                if (!gate_card->canBePlayed(state_, player)) return false;
        }
        // Fallen Feline: "opponents can't play spells with that name."
        if (gate_obj.isSpell()) {
            for (const auto& locked : state_.player(player).locked_spell_names)
                if (locked == gate_obj.name) return false;
        }
    }
    auto& card = state_.getObject(card_obj);
    if (card.card_def_id == kInvalidId) return false;  // tokens have no cost
    const auto& def = card_db_.get(card.card_def_id);

    int energy_needed = def.energy_cost;
    int power_needed = def.power_cost;

    // Apply cost reductions
    auto& ps_const = state_.player(player);
    int min_cost = 0;
    // Combat-active scope: a CostModifier with combat_active_only=true is
    // only consulted while combat is in progress on any battlefield.
    // Used by Vex Cheerless ("While I'm in combat, friendly spells cost
    // [1] less, enemy spells cost [1] more").
    bool combat_active = false;
    for (const auto& bf : state_.battlefields) {
        if (bf.combat_in_progress) { combat_active = true; break; }
    }
    // Same controller of card_obj as the cost-payer in normal play
    // (the player paying for their own play). Cross-player consult
    // (enemy cost increase) requires the modifier's source player to
    // be tracked separately — see opponent modifier sweep below.
    for (auto& mod : ps_const.cost_modifiers) {
        if (mod.next_spell_only && !card.isSpell()) continue;
        if (mod.next_unit_only && !card.isUnit()) continue;
        if (!mod.appliesToCard(card)) continue;
        if (mod.first_gear_per_turn && ps_const.gears_played_this_turn > 0) continue;
        if (mod.once_per_turn_optional && onceDiscountSpent(state_, mod)) continue;
        if (mod.affects_non_token_only && card.super_type == SuperType::Token) continue;
        if (mod.combat_active_only && !combat_active) continue;
        if (mod.affects_enemy_only) continue;  // friendly's own list, skip enemy-only
        energy_needed -= mod.energy_reduction;
        energy_needed += mod.energy_increase;
        if (mod.min_cost > min_cost) min_cost = mod.min_cost;
    }
    // Cross-player modifiers: the OPPONENT's cost_modifiers list may
    // contain affects_enemy_only entries that target THIS player. Vex
    // Cheerless (controlled by P2) wants to make P1's spells cost more.
    for (auto& mod : state_.player(opponent(player)).cost_modifiers) {
        if (!mod.affects_enemy_only) continue;
        if (mod.next_spell_only && !card.isSpell()) continue;
        if (!mod.appliesToCard(card)) continue;
        if (mod.next_unit_only && !card.isUnit()) continue;
        if (mod.affects_non_token_only && card.super_type == SuperType::Token) continue;
        if (mod.combat_active_only && !combat_active) continue;
        energy_needed -= mod.energy_reduction;
        energy_needed += mod.energy_increase;
        if (mod.min_cost > min_cost) min_cost = mod.min_cost;
    }
    // Self-cost reduction hook (e.g., Noxus Hopeful "[Legion] I cost [2] less").
    if (auto* self_card = card_registry_.get(card.card_def_id)) {
        energy_needed -= self_card->selfCostReduction(state_, player);
    }
    // Vendetta continuous spell-cost effects (set by cards' applyPassiveAura):
    // Helm of Suppression taxes the opponent's spells; Applied Researchers
    // discounts your own "to a minimum of [1]". Energy component only.
    if (card.isSpell()) {
        energy_needed += ps_const.spell_tax_energy;
        if (ps_const.spell_discount_energy > 0) {
            energy_needed -= ps_const.spell_discount_energy;
            if (min_cost < 1) min_cost = 1;
        }
    }
    // Irelia, Graceful (462): per-target discount staged in executePlaySpell when
    // this spell chose her ("your spells that choose me cost [1]/[A] less").
    energy_needed -= ps_const.transient_play_discount;
    energy_needed -= energy_discount;   // a location / spell-cost-option discount being considered
    energy_needed = std::max(min_cost, energy_needed);
    energy_needed = std::max(0, energy_needed);
    // Effect-driven play that ignores part of the cost (EffectExecutor::playPayingCost).
    if (ps_const.transient_ignore_energy) energy_needed = 0;
    power_needed = std::max(0, power_needed + powerCostDelta(state_, player, card, card_registry_.get(card.card_def_id)) - power_discount);
    if (state_.player(player).transient_ignore_power) power_needed = 0;

    // Count available runes in base, partitioned by ready/exhausted
    // and matching/non-matching-domain. The CR cost-payment ordering
    // (exhaust ready runes for energy, THEN recycle exhausted runes
    // for power) lets a single ready rune provide BOTH 1 energy and
    // 1 power: exhaust it in step 1, recycle it in step 2.
    auto base_loc = BaseLocation{player};
    int ready_total = 0;
    int matching_ready = 0;
    int matching_exhausted = 0;
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isRune() || obj.controller != player) continue;
        if (!obj.location.has_value() || *obj.location != LocationId{base_loc}) continue;
        bool matches_domain = false;
        if (power_needed > 0) {
            for (auto d : def.domains) {
                for (auto rd : obj.domains) {
                    if (d == rd) { matches_domain = true; break; }
                }
                if (matches_domain) break;
            }
        }
        if (!obj.is_exhausted) {
            ++ready_total;
            if (matches_domain) ++matching_ready;
        } else if (matches_domain) {
            ++matching_exhausted;
        }
    }

    const auto& pool = state_.player(player).rune_pool;

    // Energy can come from the pool's existing energy AND from
    // exhausting ready runes (each gives 1 energy).
    const SpendPurpose purpose = state_.purposePlaying(card.card_type);
    int energy_available = ready_total + pool.energyFor(purpose);
    if (energy_available < energy_needed) return false;

    // Power can come from:
    //   • Pool's already-spent power for any of the card's domains
    //     (per-domain bucket).
    //   • Pool's universal power.
    //   • Newly-recycled runes whose domains match the card. Recycle
    //     candidates = exhausted matching runes + matching runes we
    //     just exhausted in the energy step (the energy step exhausts
    //     `energy_needed` ready runes, and we're free to pick the
    //     matching-domain ones first when doing so).
    int power_from_pool = pool.universal_power + pool.restrictedPowerFor(purpose);
    for (auto d : def.domains) {
        int di = static_cast<int>(d);
        if (di >= 0 && di < static_cast<int>(Domain::Count)) {
            power_from_pool += pool.power[di];
        }
    }
    int power_remaining = std::max(0, power_needed - power_from_pool);
    // CR 164.2.b: a rune is recycled for Power whether it is ready or
    // exhausted, and a ready one can be exhausted for Energy first — so every
    // matching rune in base can pay Power, independently of the Energy check.
    if (matching_ready + matching_exhausted < power_remaining) return false;
    // "[A] more" extras: any rune in base can be recycled for them, on top of
    // the ones recycled for this card's own Power.
    const int extra_any = extraAnyPowerCost(state_, player, card) + extra_any_power;
    if (extra_any > 0) {
        const int any_available = payableAnyPower(state_, player, purpose) - power_needed;
        if (any_available < extra_any) return false;
    }
    return true;
}

// C-1 commit 8 — payCardCost split into a cursor-based state machine.
// The legacy payCardCost() body is now the BRIDGE: it runs the cursor
// in a queryAgent loop. The step-machine dispatch (commit 9) will
// drive the cursor directly without queryAgent.

namespace {
// Helper: enumerate ready runes in `player`'s base.
std::vector<GameObjectId> readyRunesInBase(const GameState& state, PlayerId player) {
    std::vector<GameObjectId> out;
    auto base_loc = BaseLocation{player};
    for (auto& [id, obj] : state.objects) {
        if (!obj.isRune() || obj.controller != player || obj.is_exhausted) continue;
        if (!obj.location.has_value() || *obj.location != LocationId{base_loc}) continue;
        out.push_back(id);
    }
    return out;
}
// Helper: enumerate runes (any state) in `player`'s base whose domains
// intersect `wanted`. Used for power recycling.
std::vector<GameObjectId> matchingRunesInBase(const GameState& state, PlayerId player,
                                                const std::vector<Domain>& wanted) {
    std::vector<GameObjectId> out;
    auto base_loc = BaseLocation{player};
    for (auto& [id, obj] : state.objects) {
        if (!obj.isRune() || obj.controller != player) continue;
        if (!obj.location.has_value() || *obj.location != LocationId{base_loc}) continue;
        bool match = false;
        for (auto d : wanted) {
            for (auto rd : obj.domains) {
                if (d == rd) { match = true; break; }
            }
            if (match) break;
        }
        if (match) out.push_back(id);
    }
    return out;
}
}  // namespace

GameEngine::CostPaymentAdvance GameEngine::beginCostPayment(
    PlayerId player, GameObjectId card_obj) {

    CostPaymentAdvance done;
    done.kind = CostPaymentAdvance::Kind::Done;
    done.deciding = player;

    auto& card = state_.getObject(card_obj);
    if (card.card_def_id == kInvalidId) {
        state_.cost_cursor.reset();
        return done;  // tokens cost nothing
    }
    const auto& def = card_db_.get(card.card_def_id);

    int energy_needed = def.energy_cost;
    int power_needed = def.power_cost;

    // Apply cost reductions
    auto& ps = state_.player(player);
    int min_cost = 0;
    bool combat_active = false;
    for (const auto& bf : state_.battlefields) {
        if (bf.combat_in_progress) { combat_active = true; break; }
    }
    for (auto& mod : ps.cost_modifiers) {
        if (mod.next_spell_only && !card.isSpell()) continue;
        if (mod.next_unit_only && !card.isUnit()) continue;
        if (!mod.appliesToCard(card)) continue;
        if (mod.first_gear_per_turn && ps.gears_played_this_turn > 0) continue;
        if (mod.once_per_turn_optional && onceDiscountSpent(state_, mod)) continue;
        if (mod.affects_non_token_only && card.super_type == SuperType::Token) continue;
        if (mod.combat_active_only && !combat_active) continue;
        if (mod.affects_enemy_only) continue;
        energy_needed -= mod.energy_reduction;
        energy_needed += mod.energy_increase;
        if (mod.min_cost > min_cost) min_cost = mod.min_cost;
    }
    // Cross-player modifiers (Vex Cheerless makes opponent's spells cost more).
    for (auto& mod : state_.player(opponent(player)).cost_modifiers) {
        if (!mod.affects_enemy_only) continue;
        if (mod.next_spell_only && !card.isSpell()) continue;
        if (!mod.appliesToCard(card)) continue;
        if (mod.next_unit_only && !card.isUnit()) continue;
        if (mod.affects_non_token_only && card.super_type == SuperType::Token) continue;
        if (mod.combat_active_only && !combat_active) continue;
        energy_needed -= mod.energy_reduction;
        energy_needed += mod.energy_increase;
        if (mod.min_cost > min_cost) min_cost = mod.min_cost;
    }
    // Self-cost reduction hook (e.g., Noxus Hopeful Legion discount).
    if (auto* self_card = card_registry_.get(card.card_def_id)) {
        energy_needed -= self_card->selfCostReduction(state_, player);
    }
    // A card on the player's board may give the unit being played [Accelerate]
    // (Card::grantsAccelerateTo). Mark it so resolvePermanent offers the usual
    // optional [1][C] (CR 805.1.b — a "may pay", never a mandatory surcharge).
    // The cost-payment path has no Intent, so the play source comes from the
    // transient PlayerState::current_play_source (set in executePlayCard).
    if (card.isUnit() && ps.next_unit_accelerate) {
        // "The next unit you play this turn has [Accelerate]." (Firelights' Hideout)
        ps.next_unit_accelerate = false;
        card.card_counters["__granted_accelerate"] = 1;
    }
    if (card.isUnit()) {
        const bool from_hand = ps.current_play_source == Intent::PlaySource::Hand;

        for (auto& [oid, obj] : state_.objects) {
            if (obj.controller != player || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            const Card* granter = card_registry_.get(obj.card_def_id);
            if (!granter || !granter->grantsAccelerateTo(state_, oid, card, from_hand)) continue;
            card.card_counters["__granted_accelerate"] = 1;
            break;
        }
    }
    // Vendetta continuous spell-cost effects (mirrors canAfford).
    if (card.isSpell()) {
        energy_needed += ps.spell_tax_energy;
        if (ps.spell_discount_energy > 0) {
            energy_needed -= ps.spell_discount_energy;
            if (min_cost < 1) min_cost = 1;
        }
    }
    // Irelia, Graceful (462): per-target discount staged in executePlaySpell.
    energy_needed -= ps.transient_play_discount;
    energy_needed = std::max(min_cost, energy_needed);
    energy_needed = std::max(0, energy_needed);
    if (ps.transient_ignore_energy) energy_needed = 0;
    power_needed = std::max(0, power_needed + powerCostDelta(state_, player, card, card_registry_.get(card.card_def_id)));
    if (state_.player(player).transient_ignore_power) power_needed = 0;

    // Consume one-shot modifiers that applied
    ps.cost_modifiers.erase(
        std::remove_if(ps.cost_modifiers.begin(), ps.cost_modifiers.end(),
            [&](const PlayerState::CostModifier& m) {
                if (m.next_spell_only && card.isSpell()) return true;
                if (m.next_unit_only && card.isUnit()) return true;
                if (m.next_card_only) return true;
                return false;
            }),
        ps.cost_modifiers.end());

    events_.logTrace("PAY_COST: " + card.name + " E=" + std::to_string(energy_needed) +
                     " P=" + std::to_string(power_needed));

    // Spend pool energy first (no agent decision needed): restricted Energy
    // this card may use (RunePool::restricted) before unrestricted Energy.
    const SpendPurpose purpose = state_.purposePlaying(card.card_type);
    if (energy_needed > 0) ps.rune_pool.spendEnergy(purpose, energy_needed);

    // Spend floating Power the same way: restricted [A] this card may use,
    // then this card's domains, then [A].
    if (power_needed > 0) {
        ps.rune_pool.spendRestrictedPower(purpose, power_needed);
        for (auto d : def.domains) {
            int di = static_cast<int>(d);
            if (di < 0 || di >= static_cast<int>(Domain::Count)) continue;
            int from_pool = std::min(ps.rune_pool.power[di], power_needed);
            ps.rune_pool.power[di] -= from_pool;
            power_needed -= from_pool;
        }
        int from_uni = std::min(ps.rune_pool.universal_power, power_needed);
        ps.rune_pool.universal_power -= from_uni;
        power_needed -= from_uni;
    }

    // Initialize cursor for the rune-pick phase
    GameState::CostPaymentCursor cur;
    cur.card = card_obj;
    cur.player = player;
    cur.energy_remaining = energy_needed;
    cur.power_remaining = power_needed;
    cur.power_domains = def.domains;
    cur.phase = (energy_needed > 0)
        ? GameState::CostPaymentCursor::Phase::PayingEnergy
        : ((power_needed > 0)
            ? GameState::CostPaymentCursor::Phase::PayingPower
            : GameState::CostPaymentCursor::Phase::Done);
    state_.cost_cursor = cur;

    return advanceCostPayment();
}

GameEngine::CostPaymentAdvance GameEngine::advanceCostPayment() {
    CostPaymentAdvance adv;
    if (!state_.cost_cursor.has_value()) {
        adv.kind = CostPaymentAdvance::Kind::Done;
        return adv;
    }
    auto& cur = *state_.cost_cursor;
    adv.deciding = cur.player;

    using Phase = GameState::CostPaymentCursor::Phase;

    // Skip-ahead: if energy phase is exhausted, move to power.
    if (cur.phase == Phase::PayingEnergy && cur.energy_remaining <= 0) {
        cur.phase = (cur.power_remaining > 0) ? Phase::PayingPower : Phase::Done;
    }
    // Same for power phase.
    if (cur.phase == Phase::PayingPower && cur.power_remaining <= 0) {
        cur.phase = Phase::Done;
    }

    if (cur.phase == Phase::Done) {
        state_.cost_cursor.reset();
        adv.kind = CostPaymentAdvance::Kind::Done;
        return adv;
    }

    // Energy is paid without a decision: any ready rune gives the same
    // Energy. Runes of the card's power domains go first when power is still
    // owed, so the power step can recycle the runes just exhausted instead
    // of a ready one.
    if (cur.phase == Phase::PayingEnergy) {
        std::vector<GameObjectId> ready = readyRunesInBase(state_, cur.player);
        const auto feeds_power = [&](GameObjectId id) {
            if (cur.power_remaining <= 0) return false;
            for (auto d : state_.getObject(id).domains)
                if (std::find(cur.power_domains.begin(), cur.power_domains.end(), d) !=
                    cur.power_domains.end())
                    return true;
            return false;
        };
        std::sort(ready.begin(), ready.end(), [&](GameObjectId a, GameObjectId b) {
            const bool fa = feeds_power(a), fb = feeds_power(b);
            return fa != fb ? fa : a < b;
        });
        for (GameObjectId rune_id : ready) {
            if (cur.energy_remaining <= 0) break;
            exhaustRuneForEnergy(rune_id);
        }
        if (cur.energy_remaining > 0) {
            // canAfford() was checked upstream, so this should not happen.
            state_.cost_cursor.reset();
            adv.kind = CostPaymentAdvance::Kind::Done;
            return adv;
        }
        cur.phase = (cur.power_remaining > 0) ? Phase::PayingPower : Phase::Done;
        if (cur.phase == Phase::Done) {
            state_.cost_cursor.reset();
            adv.kind = CostPaymentAdvance::Kind::Done;
            return adv;
        }
    }

    // Power: recycling an exhausted rune costs nothing more this turn, a
    // ready one also gives up its Energy, so exhausted runes are listed first.
    std::vector<GameObjectId> candidates =
        matchingRunesInBase(state_, cur.player, cur.power_domains);
    std::sort(candidates.begin(), candidates.end(), [&](GameObjectId a, GameObjectId b) {
        const bool ea = state_.getObject(a).is_exhausted;
        const bool eb = state_.getObject(b).is_exhausted;
        return ea != eb ? ea : a < b;
    });

    if (candidates.empty()) {
        // Can't continue — but the contract is canAfford() was checked
        // upstream, so we should never hit this. Defensively close out.
        state_.cost_cursor.reset();
        adv.kind = CostPaymentAdvance::Kind::Done;
        return adv;
    }

    adv.kind = CostPaymentAdvance::Kind::NeedDecision;
    adv.legal.reserve(candidates.size());
    for (auto rune_id : candidates) {
        Intent choice;
        choice.type = IntentType::MakeChoice;
        choice.player = cur.player;
        choice.chosen_objects = {rune_id};
        adv.legal.push_back(std::move(choice));
    }
    return adv;
}

void GameEngine::exhaustRuneForEnergy(GameObjectId rune_id) {
    auto& rune = state_.getObject(rune_id);
    events_.logTrace("  EXHAUST_RUNE: " + rune.name + " (id=" +
                     std::to_string(rune_id) + ") for energy");
    rune.is_exhausted = true;
    events_.emit(ObjectStateChangedEvent{rune_id, "exhausted"});
    state_.cost_cursor->energy_remaining--;
}

GameEngine::CostPaymentAdvance GameEngine::resolveCostPaymentDecision(
    const Intent& chosen) {
    assert(state_.cost_cursor.has_value() &&
           "resolveCostPaymentDecision called without an active cursor");
    auto& cur = *state_.cost_cursor;

    GameObjectId rune_id = chosen.chosen_objects.empty()
        ? kInvalidId : chosen.chosen_objects[0];
    if (rune_id == kInvalidId || !state_.objectExists(rune_id)) {
        // Defensive — invalid pick, nothing to apply
        return advanceCostPayment();
    }

    using Phase = GameState::CostPaymentCursor::Phase;
    auto& rune = state_.getObject(rune_id);

    if (cur.phase == Phase::PayingEnergy) {
        exhaustRuneForEnergy(rune_id);
    } else if (cur.phase == Phase::PayingPower) {
        events_.logTrace("  RECYCLE_RUNE: " + rune.name + " (id=" +
                         std::to_string(rune_id) + ", " +
                         (rune.is_exhausted ? "exhausted" : "ready") +
                         ") for power [agent choice]");
        auto base_loc = BaseLocation{cur.player};
        rune.location = std::nullopt;
        rune.zone = ZoneType::RuneDeck;
        rune.is_exhausted = false;
        state_.player(cur.player).rune_deck.insert(
            state_.player(cur.player).rune_deck.begin(), rune_id);
        events_.emit(LeftBoardEvent{rune_id, cur.player, CardType::Rune,
            base_loc, ZoneType::RuneDeck, false});
        cur.power_remaining--;
        state_.player(cur.player).power_spent_this_turn++;  // Sivir, Mercenary
    }

    return advanceCostPayment();
}

bool GameEngine::canPayActivationResources(PlayerId player, int energy,
                                            const ActivationCost& cost,
                                            SpendPurpose purpose) const {
    return canPayAdditionalCost(player, energy, cost.power,
        cost.power_any_domain ? Domain::Count : cost.power_domain, purpose);
}

bool GameEngine::payActivationResources(PlayerId player, int energy,
                                         const ActivationCost& cost, SpendPurpose purpose) {
    return payAdditionalCost(player, energy, cost.power,
        cost.power_any_domain ? Domain::Count : cost.power_domain, purpose);
}

// Extra (energy, power) charge on top of — or instead of — a card's base cost:
// Accelerate, Repeat, optional additional costs, Flow, activated abilities.
// `domain` is the power's Domain; Domain::Count means [A] (any domain, and the
// powers need not share one). Runes are used the way CR 164.2 lets them be:
// exhaust for Energy, recycle (ready or exhausted) for Power, and one rune can
// do both — so the two requirements are independent.
namespace {
bool runePaysPower(const GameObject& rune, Domain domain) {
    if (domain == Domain::Count) return true;
    for (auto d : rune.domains) if (d == domain) return true;
    return false;
}
std::vector<GameObjectId> runesInBase(const GameState& state, PlayerId player) {
    std::vector<GameObjectId> out;
    LocationId base{BaseLocation{player}};
    for (auto& [id, obj] : state.objects)
        if (obj.isRune() && obj.controller == player &&
            obj.location.has_value() && *obj.location == base) out.push_back(id);
    return out;
}
}  // namespace

// `purpose`: what the payment is for, which decides the restricted floating
// resources (RunePool::restricted) that may be used. Restricted Power is [A].
bool GameEngine::canPayAdditionalCost(PlayerId player, int energy, int power,
                                       Domain domain, SpendPurpose purpose) const {
    purpose.in_showdown = state_.inShowdown();
    const auto& pool = state_.player(player).rune_pool;
    int e_need = std::max(0, energy) - pool.energyFor(purpose);
    int p_need = std::max(0, power) - pool.universal_power - pool.restrictedPowerFor(purpose);
    if (domain == Domain::Count) {
        for (int di = 0; di < static_cast<int>(Domain::Count); ++di) p_need -= pool.power[di];
    } else {
        p_need -= pool.power[static_cast<int>(domain)];
    }
    if (e_need <= 0 && p_need <= 0) return true;

    int ready = 0, can_recycle = 0;
    for (auto rid : runesInBase(state_, player)) {
        const auto& rune = state_.getObject(rid);
        if (!rune.is_exhausted) ++ready;
        if (runePaysPower(rune, domain)) ++can_recycle;
    }
    return ready >= e_need && can_recycle >= p_need;
}

bool GameEngine::payAdditionalCost(PlayerId player, int energy, int power,
                                    Domain domain, SpendPurpose purpose) {
    purpose.in_showdown = state_.inShowdown();
    if (!canPayAdditionalCost(player, energy, power, domain, purpose)) return false;
    auto& ps = state_.player(player);
    int e_need = std::max(0, energy);
    int p_need = std::max(0, power);

    // Floating resources first.
    auto take = [](int& from, int& need) {
        int n = std::min(from, std::max(0, need));
        from -= n;
        need -= n;
    };
    ps.rune_pool.spendEnergy(purpose, e_need);           // restricted Energy first
    ps.rune_pool.spendRestrictedPower(purpose, p_need);  // restricted [A] first
    if (domain == Domain::Count) {
        for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
            take(ps.rune_pool.power[di], p_need);
    } else {
        take(ps.rune_pool.power[static_cast<int>(domain)], p_need);
    }
    take(ps.rune_pool.universal_power, p_need);

    auto runes = runesInBase(state_, player);

    // Energy: exhaust ready runes, keeping runes that can pay the power for
    // last (an exhausted rune can still be recycled, so this never strands one).
    std::stable_sort(runes.begin(), runes.end(), [&](GameObjectId a, GameObjectId b) {
        return runePaysPower(state_.getObject(a), domain) <
               runePaysPower(state_.getObject(b), domain);
    });
    int paid_e = 0;
    for (auto rid : runes) {
        if (e_need <= 0) break;
        auto& rune = state_.getObject(rid);
        if (rune.is_exhausted) continue;
        rune.is_exhausted = true;
        events_.emit(ObjectStateChangedEvent{rid, "exhausted"});
        --e_need;
        ++paid_e;
    }

    // Power: recycle matching runes, exhausted ones first.
    std::stable_sort(runes.begin(), runes.end(), [&](GameObjectId a, GameObjectId b) {
        return state_.getObject(a).is_exhausted > state_.getObject(b).is_exhausted;
    });
    int paid_p = 0;
    LocationId base_loc{BaseLocation{player}};
    for (auto rid : runes) {
        if (p_need <= 0) break;
        auto& rune = state_.getObject(rid);
        if (!runePaysPower(rune, domain)) continue;
        rune.location = std::nullopt;
        rune.zone = ZoneType::RuneDeck;
        rune.is_exhausted = false;
        ps.rune_deck.insert(ps.rune_deck.begin(), rid);
        ps.power_spent_this_turn++;
        events_.emit(LeftBoardEvent{rid, player, CardType::Rune,
            base_loc, ZoneType::RuneDeck, false});
        --p_need;
        ++paid_p;
    }
    events_.logTrace("PAY_ADD: " + std::string(toString(player)) + " pays [" +
                     std::to_string(energy) + "] + " + std::to_string(power) + " power (" +
                     (domain == Domain::Count ? "any" : toString(domain)) +
                     "): exhausted " + std::to_string(paid_e) + ", recycled " +
                     std::to_string(paid_p));
    return e_need <= 0 && p_need <= 0;
}


bool GameEngine::payTrashReplayGrant(PlayerId player, GameObjectId card_id) {
    auto& ps = state_.player(player);
    auto it = std::find_if(ps.trash_replay_grants.begin(),
                           ps.trash_replay_grants.end(),
                           [&](const PlayerState::TrashReplayGrant& g) {
                               return g.card == card_id;
                           });
    if (it == ps.trash_replay_grants.end()) return false;
    const auto grant = *it;   // copy before erase
    const SpendPurpose purpose = state_.objectExists(card_id)
        ? state_.purposePlaying(state_.getObject(card_id).card_type)
        : state_.purposeOther();

    // Pick a payable domain for an [A] grant; otherwise use the fixed domain.
    Domain domain = grant.power_domain;
    if (grant.any_domain) {
        bool found = false;
        for (int di = 0; di < static_cast<int>(Domain::Count); ++di) {
            if (canPayAdditionalCost(player, grant.energy, grant.power,
                                      static_cast<Domain>(di), purpose)) {
                domain = static_cast<Domain>(di);
                found = true;
                break;
            }
        }
        if (!found) return false;  // action gen should have filtered this out
    }
    bool ok = payAdditionalCost(player, grant.energy, grant.power, domain, purpose);
    if (ok && grant.banish_after && state_.objectExists(card_id))
        state_.getObject(card_id).banish_on_leave_chain = true;

    // Consume the grant whether or not payment fully succeeded — re-find since
    // payAdditionalCost may have mutated the vector indirectly (it does not,
    // but stay defensive against iterator invalidation).
    ps.trash_replay_grants.erase(
        std::remove_if(ps.trash_replay_grants.begin(),
                       ps.trash_replay_grants.end(),
                       [&](const PlayerState::TrashReplayGrant& g) {
                           return g.card == card_id;
                       }),
        ps.trash_replay_grants.end());

    if (ok) {
        events_.logTrace("TRASH_REPLAY: " + std::string(toString(player)) +
                         " replayed card id=" + std::to_string(card_id) +
                         " for [" + std::to_string(grant.energy) + "]+power");
    }
    return ok;
}

bool GameEngine::payCardCost(PlayerId player, GameObjectId card_obj) {
    // Bridge: legacy threaded path. Drives the cursor + queryAgent in
    // a loop. Same shape as the other commit-7 bridges
    // (runShowdownLoop, combatDamageStep). Step-machine dispatch
    // (commit 9) will use begin/advance/resolve directly.
    const int extra_any = state_.objectExists(card_obj)
        ? extraAnyPowerCost(state_, player, state_.getObject(card_obj)) : 0;
    auto adv = beginCostPayment(player, card_obj);
    while (adv.kind == CostPaymentAdvance::Kind::NeedDecision) {
        // Always emit the decision snapshot (forced-choice visibility
        // — see prior commit). Trace label depends on phase.
        const char* label = state_.cost_cursor->phase ==
            GameState::CostPaymentCursor::Phase::PayingEnergy
            ? "pick rune to exhaust for energy"
            : "pick rune to recycle for power";
        state_.decision_index++;
        events_.logTrace("DECISION #" +
                          std::to_string(state_.decision_index) +
                          " (" + toString(player) + "): " + label +
                          " [" + std::to_string(adv.legal.size()) +
                          " options]");

        // Always query the agent — even when only one legal option
        // exists. Previously this short-circuited and auto-picked
        // adv.legal[0], which caused the agent's policy/training
        // signal to MISS the "forced last-rune" cost-payment step
        // (the last rune in a 3E/3-runes payment). The agent still
        // returns the only option for trivial cases; HumanAgent
        // auto-passes without surfacing UI, so the player UX is
        // unchanged. The win is training fidelity.
        auto chosen = getAgent(player).selectAction(state_, adv.legal);
        recordAppliedIntent(chosen);
        if (on_decision) on_decision(state_, adv.legal, chosen);
        adv = resolveCostPaymentDecision(chosen);
    }
    if (extra_any > 0 && state_.objectExists(card_obj))
        effect_executor_->payAnyPower(player, extra_any,
            state_.purposePlaying(state_.getObject(card_obj).card_type));
    return true;
}

} // namespace riftbound
