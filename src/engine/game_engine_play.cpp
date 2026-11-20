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
// Action execution
// ═══════════════════════════════════════════════════════════════════════════════

// Energy reduction for activating ability `index` of `source`: the card's own
// (and aura-computed) reduction, plus Piltovan Forge's "the first friendly gear
// activated ability played each turn costs [1] less".
int GameEngine::activationEnergyReduction(const Card& card, PlayerId player,
                                          GameObjectId source, int index) const {
    int r = card.activationCostReductionFor(state_, player, source, index);
    const auto& ps = state_.player(player);
    if (ps.first_gear_ability_discount > 0 && ps.gear_abilities_activated_this_turn == 0 &&
        state_.objectExists(source) && state_.getObject(source).isGear())
        r += ps.first_gear_ability_discount;
    return r;
}

// CR 355: the targets of a spell or ability are chosen as it is played — when
// it becomes a finalized chain item — not as it resolves. Cards that take one
// target through Card::pickTarget (needsPlayTimeTarget / an ability's
// needs_activation_time_target) get that choice here, so the target is visible
// on the chain to both players, Deflect (CR 809) is paid now, and abilities
// that trigger on being chosen go on the chain above the item. The card's
// pickTarget then returns this choice at resolution, or nothing if it is no
// longer legal (a mistarget: the instruction is ignored).
void GameEngine::chooseTargetsAtFinalize(ChainItem& item) {
    if (item.card_def_id == kInvalidId) return;
    const bool is_trigger = item.is_ability && !item.is_activated_ability;
    bool optional_target = false;
    const Card* card = nullptr;
    std::vector<GameObjectId> legal;
    if (is_trigger) {
        // A triggered ability: its "you may", costs and targets (CR 383.3).
        // A card that opts into kFinalizeTrigger makes all of these choices
        // itself (pickTarget in its finalize pass). Otherwise a non-spell card
        // that declares target requirements gets its one target chosen here,
        // like a spell's, and reads it from onTrigger's `targets`. (`targets`
        // can also carry the triggering event's subject: then nothing is
        // chosen.) The card's 4-argument enumerateLegalTargets (ability index
        // -1, the source object) defines "here" / "another".
        const Card* tcard = card_registry_.get(item.card_def_id);
        if (!tcard) return;
        if (tcard->choicesAtFinalization() & Card::kFinalizeTrigger) {
            runFinalizePass(item);
            return;
        }
        const auto reqs = tcard->getTargetRequirements();
        if (!item.targets.empty() || reqs.count == 0 ||
            tcard->def().card_type == CardType::Spell)
            return;
        legal = tcard->enumerateLegalTargets(state_, item.controller,
                                             kTriggerAbilityIndex, item.source);
        if (legal.empty()) return;   // no target: the ability does nothing
        card = tcard;
        optional_target = reqs.optional;
    } else {
        if (!item.targets.empty()) return;
        card = (item.is_ability && item.granted_ability_def != 0)
            ? card_registry_.get(item.granted_ability_def)
            : card_registry_.get(item.card_def_id);
        if (!card) return;

        if (item.is_spell) {
            if (!card->needsPlayTimeTarget()) {
                if (card->choicesAtFinalization() & Card::kFinalizeResolve) runFinalizePass(item);
                return;
            }
            legal = card->enumerateLegalTargets(state_, item.controller);
        } else {
            auto abs = card->activatedAbilities();
            if (item.ability_index < 0 || item.ability_index >= static_cast<int>(abs.size())) return;
            if (abs[item.ability_index].is_empower) return;
            if (!abs[item.ability_index].needs_activation_time_target) {
                if (card->choicesAtFinalization() & Card::kFinalizeActivate) runFinalizePass(item);
                return;
            }
            legal = card->enumerateLegalTargets(state_, item.controller, item.ability_index, item.source);
        }
        if (legal.empty()) return;   // nothing to choose now: the card handles it as it resolves
    }

    // CR 811.1.d.2-3: played from facedown, choose among options at that battlefield.
    if (item.hidden_at.has_value() && card->hiddenPlayRestrictsTargets()) {
        std::vector<GameObjectId> here;
        for (auto t : legal)
            if (state_.choosableFromHiddenAt(t, *item.hidden_at)) here.push_back(t);
        legal = std::move(here);
        if (legal.empty()) return;
    }

    std::vector<Intent> options;
    options.reserve(legal.size());
    for (auto t : legal) {
        Intent i;
        i.type = IntentType::MakeChoice;
        i.player = item.controller;
        i.chosen_objects = {t};
        options.push_back(std::move(i));
    }
    if (optional_target) {   // "you may": choosing nothing declines
        Intent none;
        none.type = IntentType::MakeChoice;
        none.player = item.controller;
        options.push_back(std::move(none));
    }
    const std::string source_name = state_.objectExists(item.source)
        ? state_.getObject(item.source).name : std::string("?");
    state_.decision_index++;
    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                     toString(item.controller) + "): choose a target for " + source_name +
                     " [" + std::to_string(options.size()) + " options]");
    const PlayerId chooser = item.controller;
    const ChainItemId item_id = item.id;
    auto chosen = getAgent(chooser).selectAction(state_, options);
    recordAppliedIntent(chosen);
    if (on_decision) on_decision(state_, options, chosen);
    if (optional_target && chosen.chosen_objects.empty()) {
        events_.logTrace("TARGET (finalize): " + source_name + " -> none (declined)");
        return;
    }
    GameObjectId t = legal.front();
    if (!chosen.chosen_objects.empty() &&
        std::find(legal.begin(), legal.end(), chosen.chosen_objects.front()) != legal.end())
        t = chosen.chosen_objects.front();

    // The agent call may have reallocated the chain: find the item again.
    ChainItem* it = nullptr;
    for (auto& ci : state_.chain.items) if (ci.id == item_id) { it = &ci; break; }
    if (!it) return;
    it->targets = {t};
    it->chosen_objects.push_back(t);
    while (it->resume_data.size() < 6) it->resume_data.push_back(0);
    it->resume_data[5] = 1;   // "target was chosen at finalization" (read by Card::pickTarget)
    events_.logTrace("TARGET (finalize): " + source_name + " -> " + state_.getObject(t).name);

    // CR 809: Deflect is a mandatory additional cost of choosing the object.
    if (!card->ignoresDeflect()) {
        const int deflect = deflectCostFor(state_, chooser, t);
        if (deflect > 0) {
            effect_executor_->payAnyPower(chooser, deflect, state_.purposeOf(*it));
            events_.logTrace("DEFLECT: " + std::string(toString(chooser)) + " pays " +
                             std::to_string(deflect) + " [A] to choose " +
                             state_.getObject(t).name);
        }
    }
    const auto& tobj = state_.getObject(t);
    if (tobj.isUnit() && tobj.location.has_value()) {
        if (tobj.controller == chooser)
            events_.emit(ObjectStateChangedEvent{t, "chosen_by_controller"});
        else
            state_.player(chooser).chose_enemy_unit_this_turn = true;
    }
}

Intent GameEngine::decideNow(PlayerId player, const std::vector<Intent>& options,
                             const std::string& what) {
    state_.decision_index++;
    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                     toString(player) + "): " + what + " [" +
                     std::to_string(options.size()) + " options]");
    auto chosen = getAgent(player).selectAction(state_, options);
    recordAppliedIntent(chosen);
    if (on_decision) on_decision(state_, options, chosen);
    return chosen;
}

// CR 750-755: the item's (new) controller re-chooses its targets. Nothing is
// re-chosen when there is no legal choice to make (753.2), and the choices
// obey the same requirements as the originals (753.3).
void GameEngine::makeNewChoicesFor(ChainItemId item_id) {
    ChainItem* item = nullptr;
    for (auto& ci : state_.chain.items) if (ci.id == item_id) { item = &ci; break; }
    if (!item || item->card_def_id == kInvalidId) return;
    const Card* card = card_registry_.get(item->card_def_id);
    if (!card) return;
    const PlayerId chooser = item->controller;
    const auto legal = card->enumerateLegalTargets(state_, chooser);

    if (item->is_spell && card->needsPlayTimeTarget()) {
        if (legal.empty()) return;
        item->targets.clear();
        item->chosen_objects.clear();
        item->resume_point = 0;
        item->resume_data.clear();
        chooseTargetsAtFinalize(*item);
        return;
    }
    if (card->needsPlayTimeTargetPair() || card->choicesAtFinalization() != 0) {
        // Chosen through the card's own helpers: forget the answers, and the
        // new controller is asked as the item resolves.
        item->targets.clear();
        item->chosen_objects.clear();
        item->resume_point = 0;
        item->resume_data.clear();
        item->may_answer = -1;
        item->choices_finalized = false;
        item->finalize_checked = 0;
        return;
    }
    // Targets locked in as the spell was played: choose that many again.
    const size_t needed = item->targets.size();
    if (needed == 0 || legal.size() < needed) return;
    std::vector<GameObjectId> remaining = legal, picked;
    while (picked.size() < needed) {
        std::vector<Intent> options;
        for (auto t : remaining) {
            Intent i;
            i.type = IntentType::MakeChoice;
            i.player = chooser;
            i.chosen_objects = {t};
            options.push_back(std::move(i));
        }
        const Intent chosen = decideNow(chooser, options, "make a new choice");
        GameObjectId t = remaining.front();
        if (!chosen.chosen_objects.empty() &&
            std::find(remaining.begin(), remaining.end(), chosen.chosen_objects.front()) !=
                remaining.end())
            t = chosen.chosen_objects.front();
        picked.push_back(t);
        remaining.erase(std::remove(remaining.begin(), remaining.end(), t), remaining.end());
    }
    // decideNow may have reallocated the chain.
    for (auto& ci : state_.chain.items)
        if (ci.id == item_id) { ci.targets = picked; ci.chosen_objects = picked; }
}

// Runs an opted-in card's callback once as its chain item is finalized, so the
// choice helpers (confirmOptional / pickMode / pickTarget / pickTargetPair) ask
// the agent now and costs are paid now; the card returns at its
// Card::finalizingChoices marker, before the effect. The answers are written
// back to the item, and the helpers return them when it resolves.
void GameEngine::runFinalizePass(ChainItem& item) {
    // An item is mid-resolution (this one was added by its effect): the
    // resume slot is taken, so this item decides as it resolves instead.
    if (state_.chain.resuming.has_value()) return;
    const ChainItemId item_id = item.id;
    state_.chain.resuming = item;
    state_.chain.resuming->finalizing = true;

    constexpr int kMaxChoices = 16;
    for (int iter = 0; iter < kMaxChoices; ++iter) {
        resolveSpell(*state_.chain.resuming);
        if (!effect_executor_->hasPendingChoice()) break;
        auto pending = effect_executor_->consumePendingChoice();
        events_.logTrace(std::string("CHOICE-REQUEST (finalize): ") +
                         (pending.label.empty() ? "MakeChoice" : pending.label) + " [" +
                         std::to_string(pending.legal.size()) + " options] (" +
                         toString(pending.player) + ")");
        Intent choice = queryAgentForChain(pending.player, pending.legal);
        effect_executor_->recordChoice(std::move(choice));
    }

    ChainItem done = std::move(*state_.chain.resuming);
    state_.chain.resuming.reset();
    for (auto& ci : state_.chain.items) {
        if (ci.id != item_id) continue;
        ci.resume_point = done.resume_point;
        ci.resume_data = std::move(done.resume_data);
        ci.chosen_objects = std::move(done.chosen_objects);
        ci.may_answer = done.may_answer;
        ci.finalize_costs_paid = done.finalize_costs_paid;
        ci.finalize_checked = 0;
        ci.choices_finalized = true;
        break;
    }
}

// Pay an activated ability's costs and put it on the chain. Returns true if a
// chain item was added (the caller then runs or resumes the chain); false if
// the activation completed without one (Equip) or was not possible.
bool GameEngine::payAndQueueAbility(const Intent& intent) {
    auto& source = state_.getObject(intent.ability_source);
    // Skip card-def lookup for tokens (no CardDef). The rest of this
    // handler reads from `source` directly; the previous `def` binding
    // was dead and threw std::out_of_range when source was a token.
    if (source.card_def_id == kInvalidId) return false;

    // Equip: card handles its own cost payment and attachment.
    // Phase 6r — gears with needsEquipTimeTarget=true get an
    // empty-targets intent here; onEquip uses pickTarget to
    // resolve. We pass kInvalidId as the unit id; the gear's
    // onEquip override is responsible for the pick.
    if (source.isGear() && !source.attached_to.has_value()) {
        Card* gear_card = card_registry_.get(source.card_def_id);
        // Equip paid by exhausting a friendly unit: targets = {unit, unit to exhaust}.
        if (gear_card && gear_card->hasEquipAbility() && gear_card->equipByExhaustingAUnit() &&
            intent.targets.size() >= 2) {
            const GameObjectId unit = intent.targets[0], payer = intent.targets[1];
            if (!state_.objectExists(unit) || !state_.objectExists(payer)) return false;
            const auto& p = state_.getObject(payer);
            if (!p.isUnit() || p.controller != intent.player || p.is_exhausted ||
                !p.location.has_value())
                return false;
            effect_executor_->exhaustObject(payer);
            events_.logTrace("EQUIP_COST: exhausted " + p.name + " to equip " + source.name);
            CardContext equip_ctx{state_, events_, *effect_executor_,
                                 intent.player, intent.ability_source};
            if (state_.objectExists(unit) && gear_card->onEquipPaid(equip_ctx, unit)) cleanup();
            return false;
        }
        if (gear_card && gear_card->hasEquipAbility()) {
            bool has_target = !intent.targets.empty();
            if (has_target || gear_card->needsEquipTimeTarget()) {
                CardContext equip_ctx{state_, events_, *effect_executor_,
                                     intent.player, intent.ability_source};
                GameObjectId target = has_target ? intent.targets[0]
                                                  : kInvalidId;
                if (gear_card->onEquip(equip_ctx, target)) {
                    cleanup();
                }
                return false;
            }
        }
    }

    // Activated ability: exhaust source, pay cost, add to chain

    // Pay activation cost via Card object. Phase 6r — read the
    // per-ability cost from activatedAbilities()[intent.ability_index]
    // so multi-ability cards pay the right cost for the right
    // ability variant. Single-ability cards have ability_index=0
    // (default) and the wrapping default impl returns their legacy
    // getActivationCost() as the only element.
    Card* ability_card = card_registry_.get(source.card_def_id);
    // Aura-granted ability: the COST descriptor comes from the granting
    // card, but the cost is paid by the BEARER (source) below.
    Card* cost_card = (intent.granted_ability_def != 0)
        ? card_registry_.get(intent.granted_ability_def) : ability_card;
    ActivationCost act_cost;
    if (cost_card) {
        auto abilities = cost_card->activatedAbilities();
        int idx = intent.ability_index;
        if (idx < 0 || idx >= static_cast<int>(abilities.size())) idx = 0;
        if (!abilities.empty()) act_cost = abilities[idx].cost;
        // State-aware energy reduction (Bashful Bloom, Risen Altar), clamped ≥ 0.
        // Risen Altar's part ("[Empower] costs ... cost [1] or [A] less") may
        // come off the Power instead: the player chooses when the cost has both.
        int red = activationEnergyReduction(*cost_card, intent.player, intent.ability_source, idx);
        const int altar = (!abilities.empty() && abilities[idx].is_empower && intent.granted_ability_def == 0)
            ? std::min(red, source.empower_cost_reduction) : 0;
        red -= altar;
        if (red > 0) act_cost.energy = std::max(0, act_cost.energy - red);
        if (altar > 0) {
            bool off_power = act_cost.energy == 0 && act_cost.power > 0;
            if (act_cost.energy > 0 && act_cost.power > 0) {
                Intent e; e.type = IntentType::MakeChoice; e.player = intent.player; e.chosen_value = 0;
                Intent p; p.type = IntentType::MakeChoice; p.player = intent.player; p.chosen_value = 1;
                std::vector<Intent> opts;
                if (canPayActivationResources(intent.player, std::max(0, act_cost.energy - altar), act_cost,
                                              state_.purposeAbilityOf(source.card_type)))
                    opts.push_back(e);
                ActivationCost less_power = act_cost;
                less_power.power = std::max(0, act_cost.power - altar);
                if (canPayActivationResources(intent.player, act_cost.energy, less_power,
                                              state_.purposeAbilityOf(source.card_type)))
                    opts.push_back(p);
                if (opts.size() == 2) {
                    state_.decision_index++;
                    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                                     toString(intent.player) + "): Risen Altar: [1] or [A] less?");
                    auto chosen = getAgent(intent.player).selectAction(state_, opts);
                    recordAppliedIntent(chosen);
                    if (on_decision) on_decision(state_, opts, chosen);
                    off_power = chosen.chosen_value.value_or(0) == 1;
                } else if (opts.size() == 1) {
                    off_power = opts[0].chosen_value.value_or(0) == 1;
                }
            }
            if (off_power) act_cost.power = std::max(0, act_cost.power - altar);
            else act_cost.energy = std::max(0, act_cost.energy - altar);
        }
    }
    if (act_cost.exhaust) {
        source.is_exhausted = true;
        events_.emit(ObjectStateChangedEvent{intent.ability_source, "exhausted"});
    }
    // "Disempower this," as a cost (Questionable Tome, Hextech Disc, ...).
    if (act_cost.disempower_self)
        effect_executor_->disempower(intent.ability_source);
    // "Spend my buff:" as a cost (Sett, Brawler; Udyr, Wildman).
    if (act_cost.spend_buff_self && source.buff_count > 0) {
        events_.logTrace("ACTIVATE_COST: " + source.name + " spends a buff");
        effect_executor_->spendBuff(intent.ability_source);
    }
    // "Kill a friendly unit" as a cost: the chosen unit rides in targets.
    if (act_cost.kill_friendly_unit && !intent.targets.empty() &&
        state_.objectExists(intent.targets[0]))
        killUnit(intent.targets[0]);
    // Pay energy (+ power) cost. Abilities with a power component pay
    // both through the shared resource path (floating pool, then runes).
    if (act_cost.power > 0 || act_cost.energy > 0)
        payActivationResources(intent.player, act_cost.energy, act_cost,
                               state_.purposeAbilityOf(source.card_type));
    // Pay recycle-self cost [C]
    if (act_cost.recycle_self) {
        events_.logTrace("ACTIVATE_COST: recycle self " + source.name);
        source.zone = ZoneType::MainDeck;
        source.location = std::nullopt;
        state_.player(intent.player).main_deck.insert(
            state_.player(intent.player).main_deck.begin(), intent.ability_source);
    }
    // Pay discard cost
    if (act_cost.discard && act_cost.discard_count > 0) {
        events_.logTrace("ACTIVATE_COST: discard " + std::to_string(act_cost.discard_count));
        if (act_cost.discard_type_filter == 0) {
            effect_executor_->discardCards(intent.player, act_cost.discard_count);
        } else {
            // Typed discard ("Discard a spell" / "Discard a gear"): the
            // first matching cards in hand.
            std::vector<GameObjectId> picks;
            for (auto hid : state_.player(intent.player).hand) {
                if (static_cast<int>(picks.size()) >= act_cost.discard_count) break;
                if (static_cast<int>(state_.getObject(hid).card_type) + 1 ==
                        act_cost.discard_type_filter) picks.push_back(hid);
            }
            for (auto hid : picks) effect_executor_->applyDiscard(intent.player, hid);
        }
    }
    // Pay XP cost
    if (act_cost.xp_cost > 0) {
        state_.player(intent.player).xp -= act_cost.xp_cost;
        events_.logTrace("ACTIVATE_COST: spend " +
                          std::to_string(act_cost.xp_cost) + " XP");
    }

    // Add ability to chain and resolve. is_activated=true so
    // stepResolve dispatches through Card::onActivate (not
    // onTrigger). Triggered abilities flow through TriggerManager
    // which calls addAbility with the default is_activated=false.
    {
        auto& aps = state_.player(intent.player);
        aps.abilities_activated_this_turn++;
        if (source.isGear()) aps.gear_abilities_activated_this_turn++;
    }
    chain_manager_->addAbility(intent.ability_source, intent.player,
                                source.card_def_id, intent.targets,
                                /*is_activated=*/true,
                                intent.ability_index);
    // Carry the granted-ability source onto the chain item so resolution
    // dispatches to the granting card's onActivate (Forge/Gardens/Heimer).
    if (intent.granted_ability_def != 0 && !state_.chain.items.empty())
        state_.chain.items.back().granted_ability_def = intent.granted_ability_def;
    if (state_.objectExists(intent.ability_source)) {
        auto& src = state_.getObject(intent.ability_source);
        // "Only once each turn" abilities count the activation, not the
        // resolution: a countered activation is still this turn's use.
        src.card_counters["__ability_used_turn_" + std::to_string(intent.ability_index)] =
            state_.turn.turn_number + 1;
        // ... and by whom, for abilities each player may use (Black Market).
        src.card_counters["__ability_used_turn_p" +
                          std::to_string(static_cast<int>(intent.player)) + "_" +
                          std::to_string(intent.ability_index)] = state_.turn.turn_number + 1;

        // "When you play ... an activated ability with Energy cost N" (the
        // printed cost, before reductions).
        int printed_energy = 0;
        if (cost_card) {
            auto abilities = cost_card->activatedAbilities();
            if (intent.ability_index >= 0 &&
                intent.ability_index < static_cast<int>(abilities.size()))
                printed_energy = abilities[intent.ability_index].cost.energy;
        }
        src.card_counters["__ability_energy_cost"] = printed_energy;
        events_.emit(ObjectStateChangedEvent{intent.ability_source, "ability_activated"});
    }
    return true;
}

void GameEngine::executeIntent(const Intent& intent) {
    events_.logTrace(std::string("INTENT: ") + toString(intent.type) + " by " + toString(intent.player));

    switch (intent.type) {
        case IntentType::PlayCard:
        case IntentType::PlayActionCard:
        case IntentType::PlayReaction: {   // e.g. a pounce in a showdown (Rengar, Pouncing)
            auto& card = state_.getObject(intent.card);
            if (card.is_hidden) {
                executePlayFromHidden(intent);
            } else if (card.isSpell()) {
                executePlaySpell(intent);
            } else {
                executePlayCard(intent);
            }
            break;
        }
        case IntentType::ActivateAbility:
        case IntentType::ActivateActionAbility: {
            if (payAndQueueAbility(intent)) runChain();
            break;
        }
        case IntentType::StandardMove:
            executeStandardMove(intent);
            break;
        case IntentType::EndTurn:
            executeEndTurn(intent);
            break;
        case IntentType::MulliganDecision:
            executeMulligan(intent);
            break;
        case IntentType::AssignCombatDamage:
            executeAssignCombatDamage(intent);
            break;
        case IntentType::PassFocus:
            executePassFocus(intent);
            break;
        case IntentType::HideCard:
            executeHideCard(intent);
            break;
        case IntentType::Concede:
            state_.game_over = true;
            state_.winner = opponent(intent.player);
            state_.game_over_reason = toString(intent.player) +
                                       std::string(" conceded");
            events_.emit(GameOverEvent{state_.winner, state_.game_over_reason});
            break;
        default:
            break;
    }
}

void GameEngine::executePlayCard(const Intent& intent) {
    auto& ps = state_.player(intent.player);
    auto& card = state_.getObject(intent.card);
    int trace_cost = (card.card_def_id != kInvalidId)
        ? card_db_.get(card.card_def_id).energy_cost : 0;
    events_.logTrace("PLAY: " + card.name + " (id=" + std::to_string(intent.card) +
                     ", " + toString(card.card_type) + ", cost=" +
                     std::to_string(trace_cost) + "E)");

    // Remove from current zone (hand or champion zone) (CR 354: step 1)
    if (card.zone == ZoneType::Hand) {
        auto it = std::find(ps.hand.begin(), ps.hand.end(), intent.card);
        if (it != ps.hand.end()) ps.hand.erase(it);
    } else if (card.zone == ZoneType::ChampionZone) {
        ps.champion_zone = kInvalidId;
    } else if (card.zone == ZoneType::Trash && intent.play_source == Intent::PlaySource::Trash) {
        ps.trash.erase(std::remove(ps.trash.begin(), ps.trash.end(), intent.card), ps.trash.end());
    } else if (card.zone == ZoneType::Banishment &&
               intent.play_source == Intent::PlaySource::Banishment) {
        ps.banishment.erase(std::remove(ps.banishment.begin(), ps.banishment.end(), intent.card),
                            ps.banishment.end());
        auto& gs = ps.banish_play_grants;
        gs.erase(std::remove_if(gs.begin(), gs.end(),
                                [&](const auto& g) { return g.card == intent.card; }), gs.end());
    }


    // Pay the card's cost (CR 357). Surface the play_source via a
    // transient field on PlayerState so the cost path can consult it
    // for Rek'Sai's auto-Accelerate (and any future from-non-hand
    // discounts). Always clear after the call.
    // Brazen Buccaneer (002): "you may discard 1 as an additional cost; if you
    // do, reduce my cost by [2]." Decided BEFORE payment so the discount applies;
    // staged into transient_play_discount (consumed by payCardCost) and cleared
    // after. Skipped on an alternative-cost play.
    int prepay_discount = 0;
    int prepay_power = 0;
    // A battlefield's discount for a unit or gear played to it (Hunters' Circle).
    if (!intent.use_alt_play_cost && intent.play_location.has_value() &&
        std::holds_alternative<BattlefieldLocation>(*intent.play_location))
        prepay_discount += playHereDiscount(
            intent.player, std::get<BattlefieldLocation>(*intent.play_location).id, card);
    if (!intent.use_alt_play_cost) {
        const Card* pc = card.card_def_id != kInvalidId
            ? card_registry_.get(card.card_def_id) : nullptr;
        Card::OptionalAdditionalCost oac = pc ? pc->optionalAdditionalCost()
                                              : Card::OptionalAdditionalCost{};
        // Poppy: "you may spend 3 XP as an additional cost; if you do, I cost [3] less".
        if (oac.valid && (oac.discard_cards > 0 || oac.spend_xp > 0) && oac.reduce_energy > 0 &&
            static_cast<int>(ps.hand.size()) >= oac.discard_cards && ps.xp >= oac.spend_xp) {
            Intent decline; decline.type = IntentType::MakeChoice;
            decline.player = intent.player; decline.chosen_value = 0;
            Intent accept; accept.type = IntentType::MakeChoice;
            accept.player = intent.player; accept.chosen_value = 1;
            std::vector<Intent> opts = {decline, accept};
            // Taken without asking when the card can't be paid for otherwise.
            Intent chosen = accept;
            if (canAfford(intent.player, intent.card, 0, prepay_discount)) {
                state_.decision_index++;
                events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                    " (" + toString(intent.player) + "): " +
                    (oac.spend_xp > 0 ? "spend " + std::to_string(oac.spend_xp) + " XP"
                                      : "discard " + std::to_string(oac.discard_cards)) +
                    " to reduce " + card.name +
                    " by " + std::to_string(oac.reduce_energy) + "? [decline|discard]");
                chosen = getAgent(intent.player).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);
            }
            if (chosen.chosen_value.value_or(0) == 1) {
                if (oac.discard_cards > 0) effect_executor_->discardCards(intent.player, oac.discard_cards);
                ps.xp -= oac.spend_xp;
                prepay_discount += oac.reduce_energy;
                if (oac.paid_flag && oac.paid_flag[0] != '\0')
                    card.card_counters[oac.paid_flag] = 1;
                events_.logTrace("ADDITIONAL_COST: " + card.name +
                    " discarded to reduce cost by " + std::to_string(prepay_discount));
            }
        }
    }
    // "You may kill a friendly unit / return a friendly gear as an additional
    // cost to play me" (Shimmering Swimmer, Dune Devourer, Skystrike Squadron).
    if (!intent.use_alt_play_cost) {
        const Card* pc = card.card_def_id != kInvalidId
            ? card_registry_.get(card.card_def_id) : nullptr;
        Card::OptionalAdditionalCost oac = pc ? pc->optionalAdditionalCost()
                                              : Card::OptionalAdditionalCost{};
        if (oac.valid && oac.kill_any_number) {
            // One unit at a time until the player stops; stopping is offered
            // once the card is affordable with the kills so far.
            while (true) {
                std::vector<GameObjectId> fodder;
                for (const auto& [oid, o] : state_.objects)
                    if (oid != intent.card && o.controller == intent.player && o.isUnit() &&
                        o.location.has_value() && pc->additionalCostAccepts(state_, o))
                        fodder.push_back(oid);
                if (fodder.empty()) break;
                std::vector<Intent> opts;
                if (canAfford(intent.player, intent.card, 0, prepay_discount, prepay_power)) {
                    Intent stop; stop.type = IntentType::MakeChoice; stop.player = intent.player;
                    stop.chosen_value = 0;
                    opts.push_back(stop);
                }
                for (auto f : fodder) {
                    Intent c; c.type = IntentType::MakeChoice; c.player = intent.player;
                    c.chosen_objects = {f};
                    opts.push_back(c);
                }
                state_.decision_index++;
                events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                    toString(intent.player) + "): kill a friendly unit to reduce " + card.name +
                    "? [" + std::to_string(opts.size()) + " options]");
                auto chosen = getAgent(intent.player).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);
                if (chosen.chosen_objects.empty() || !state_.objectExists(chosen.chosen_objects[0])) break;
                effect_executor_->killObject(chosen.chosen_objects[0]);
                prepay_power += oac.power_reduction_per_kill;
                events_.logTrace("ADDITIONAL_COST: killed a unit for " + card.name);
            }
        } else if (oac.valid && (oac.kill_friendly_unit || oac.kill_friendly_gear || oac.return_friendly_gear)) {
            std::vector<GameObjectId> fodder;
            for (const auto& [oid, o] : state_.objects) {
                if (oid == intent.card || o.controller != intent.player || !o.location.has_value()) continue;
                if (o.attached_to.has_value() && !oac.return_friendly_gear && !oac.kill_friendly_gear) continue;
                if (((oac.kill_friendly_unit && o.isUnit()) ||
                     ((oac.kill_friendly_gear || oac.return_friendly_gear) && o.isGear())) &&
                    pc->additionalCostAccepts(state_, o))
                    fodder.push_back(oid);
            }
            // Required when printed so, or when the card can't be paid for
            // without it; then only fodder that makes it affordable is offered.
            const bool needed = oac.required ||
                !canAfford(intent.player, intent.card, 0, prepay_discount, prepay_power);
            if (needed && !oac.required) {
                std::vector<GameObjectId> enough;
                for (auto f : fodder) {
                    const auto d = additionalCostDiscount(oac, f);
                    if (canAfford(intent.player, intent.card, 0, prepay_discount + d.first,
                                  prepay_power + d.second))
                        enough.push_back(f);
                }
                fodder = enough;
            }
            if (!fodder.empty()) {
                std::vector<Intent> opts;
                Intent decline; decline.type = IntentType::MakeChoice;
                decline.player = intent.player; decline.chosen_value = 0;
                if (!needed) opts.push_back(decline);
                for (auto f : fodder) {
                    Intent c; c.type = IntentType::MakeChoice; c.player = intent.player;
                    c.chosen_objects = {f};
                    opts.push_back(c);
                }
                state_.decision_index++;
                events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                    " (" + toString(intent.player) + "): " +
                    (oac.return_friendly_gear ? "return a friendly gear" : "kill a friendly object") +
                    " as an additional cost to play " + card.name + "? [" +
                    std::to_string(opts.size()) + " options]");
                auto chosen = getAgent(intent.player).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);
                if (!chosen.chosen_objects.empty() && state_.objectExists(chosen.chosen_objects[0])) {
                    const auto f = chosen.chosen_objects[0];
                    const auto d = additionalCostDiscount(oac, f);   // before it leaves the board
                    if (oac.return_friendly_gear) effect_executor_->bounceToHand(f);
                    else effect_executor_->killObject(f);
                    prepay_discount += d.first;
                    prepay_power += d.second;
                    if (oac.paid_flag && oac.paid_flag[0] != '\0')
                        card.card_counters[oac.paid_flag] = 1;
                    events_.logTrace("ADDITIONAL_COST: paid for " + card.name);
                }
            }
        }
    }
    // "Once each turn, you may pay [..] less" discounts (Orianna): the player
    // decides now; taken without asking when the card can't be paid for otherwise.
    if (!intent.use_alt_play_cost) {
        const auto mods = ps.cost_modifiers;
        for (const auto& mod : mods) {
            if (!mod.once_per_turn_optional || mod.affects_enemy_only) continue;
            if (!mod.appliesToCard(card) || onceDiscountSpent(state_, mod)) continue;
            auto& src = state_.getObject(mod.source);
            src.card_counters["__discount_turn"] = state_.turn.turn_number;   // spent, tentatively
            bool use = !canAfford(intent.player, intent.card, 0,
                                  prepay_discount);
            if (!use) {
                Intent decline; decline.type = IntentType::MakeChoice;
                decline.player = intent.player; decline.chosen_value = 0;
                Intent accept = decline; accept.chosen_value = 1;
                std::vector<Intent> opts = {decline, accept};
                state_.decision_index++;
                events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                                 toString(intent.player) + "): use " + src.name +
                                 "'s once-a-turn discount on " + card.name + "?");
                auto chosen = getAgent(intent.player).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);
                use = chosen.chosen_value.value_or(0) == 1;
            }
            if (!use) { src.card_counters.erase("__discount_turn"); continue; }
            prepay_discount += mod.energy_reduction;
            prepay_power += mod.power_reduction;
            events_.logTrace("DISCOUNT: " + src.name + " reduces the cost of " + card.name);
        }
    }
    ps.transient_play_discount += prepay_discount;
    ps.transient_power_discount += prepay_power;

    ps.current_play_source = intent.play_source;
    if (intent.use_alt_play_cost) {
        // Alternative play cost (Jhin, Meticulous Killer: "play me for [B]"):
        // pay the card's alternativePlayCost instead of the printed cost.
        const Card* c = (card.card_def_id != kInvalidId)
            ? card_registry_.get(card.card_def_id) : nullptr;
        Card::AltPlayCost alt = c ? c->alternativePlayCost(state_, intent.player)
                                  : Card::AltPlayCost{};
        Domain d = alt.power_domain;
        if (alt.any_domain) {
            for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
                if (canPayAdditionalCost(intent.player, alt.energy, alt.power,
                                          static_cast<Domain>(di),
                                          state_.purposePlaying(card.card_type))) {
                    d = static_cast<Domain>(di); break;
                }
        }
        payAdditionalCost(intent.player, alt.energy, alt.power, d,
                          state_.purposePlaying(card.card_type));
        events_.logTrace("ALT_COST: " + card.name + " played for its alternative cost");
    } else {
        payCardCost(intent.player, intent.card);
    }
    ps.current_play_source = Intent::PlaySource::Hand;
    ps.transient_play_discount -= prepay_discount;  // un-stage Brazen's discount
    ps.transient_power_discount -= prepay_power;

    // A battlefield's pay-to-play-here additional cost (Dragon Roost).
    if (intent.pay_location_cost && intent.play_location.has_value() &&
        std::holds_alternative<BattlefieldLocation>(*intent.play_location)) {
        const auto bf_id = std::get<BattlefieldLocation>(*intent.play_location).id;
        for (const auto& bf : state_.battlefields) {
            if (bf.id != bf_id || bf.paid_play_power <= 0) continue;
            effect_executor_->payAnyPower(intent.player, bf.paid_play_power,
                                          state_.purposePlaying(card.card_type));
            events_.logTrace("ADDITIONAL_COST: " + card.name + " pays " +
                             std::to_string(bf.paid_play_power) + " [A] to be played to BF#" +
                             std::to_string(bf_id));
        }
    }

    // "You may pay X as an additional cost to play me" (Akshan, Nami). Paid
    // here — after the base cost, before CardPlayedEvent fires WhenYouPlayMe —
    // so the card's play trigger can read card_counters[paid_flag].
    maybePayOptionalAdditionalCost(intent.player, intent.card);

    // Track play count
    ps.cards_played_this_turn++;
    if ((card.isUnit() || card.isGear()) && intent.play_location.has_value() &&
        std::holds_alternative<BattlefieldLocation>(*intent.play_location)) {
        ps.played_permanent_to_bf_this_turn = true;
        ps.bfs_played_permanent_this_turn.push_back(
            std::get<BattlefieldLocation>(*intent.play_location).id);
    }
    if (card.isUnit()) ps.next_unit_any_bf = false;   // Last Caress: "the next unit"


    // Per-turn gear/equipment counters (Emperor of the Sands gate; Ornn's
    // Forge "first gear each turn"). NOTE: this runs AFTER payCardCost, so the
    // first gear's cost reduction still sees gears_played_this_turn == 0.
    if (card.isGear()) {
        ps.gears_played_this_turn++;
        for (const auto& tag : card.tags) {
            if (tag == "Equipment") { ps.equipment_played_this_turn++; break; }
        }
    }
    int energy_spent = 0;
    if (card.card_def_id != kInvalidId) {
        energy_spent = card_db_.get(card.card_def_id).energy_cost;
    }
    events_.emit(CardPlayedEvent{intent.card, intent.player,
        card.card_type, ps.cards_played_this_turn, energy_spent});
    if (intent.play_source != Intent::PlaySource::Hand)
        events_.emit(ObjectStateChangedEvent{intent.card, "played_not_from_hand"});

    // Store the play location on the game object so resolvePermanent can use it.
    // Permanents choose location during finalization (CR 355.2.a).
    card.location = intent.play_location.value_or(BaseLocation{intent.player});

    // CR 135.2.b.3 + CR 355.1 — "As you play me" / "As I am played"
    // instructions execute during the play action itself, not as a
    // triggered ability. No chain item is created; no priority window
    // opens between this hook and the permanent landing on the board.
    // The card can mutate state (spawn tokens, change its own location)
    // before resolvePermanent emits EnteredBoardEvent. Used by Baron
    // Nashor to spawn the Baron Pit BF token and redirect itself there.
    if (card.card_def_id != kInvalidId) {
        if (Card* card_obj = card_registry_.get(card.card_def_id)) {
            CardContext on_play_ctx{state_, events_, *effect_executor_,
                                     intent.player, intent.card};
            card_obj->onPlay(on_play_ctx);
        }
    }

    // Route through chain — permanent resolves immediately at Finalize (CR 337.1.c)
    chain_manager_->addPermanent(intent.card, intent.player);
    runChain();
}

void GameEngine::executePlaySpell(const Intent& intent) {
    auto& ps = state_.player(intent.player);
    auto& card = state_.getObject(intent.card);
    {
        std::string tgt_str;
        for (auto t : intent.targets) {
            if (!tgt_str.empty()) tgt_str += ",";
            tgt_str += state_.objectExists(t) ? state_.getObject(t).name : "?";
            tgt_str += "(id=" + std::to_string(t) + ")";
        }
        events_.logTrace("SPELL: " + card.name + " (id=" + std::to_string(intent.card) +
                         ") targets=[" + tgt_str + "]");
    }

    // Remove from the source zone.
    if (card.zone == ZoneType::Hand) {
        auto it = std::find(ps.hand.begin(), ps.hand.end(), intent.card);
        if (it != ps.hand.end()) ps.hand.erase(it);
    } else if (intent.play_source == Intent::PlaySource::Trash &&
               card.zone == ZoneType::Trash) {
        // Trash-replay (Death from Below): play the spell straight out of trash.
        auto it = std::find(ps.trash.begin(), ps.trash.end(), intent.card);
        if (it != ps.trash.end()) ps.trash.erase(it);
    } else if (intent.play_source == Intent::PlaySource::Banishment &&
               card.zone == ZoneType::Banishment) {
        // A banished card its player may play (Runeblade Anvil).
        ps.banishment.erase(std::remove(ps.banishment.begin(), ps.banishment.end(), intent.card),
                            ps.banishment.end());
        auto& gs = ps.banish_play_grants;
        gs.erase(std::remove_if(gs.begin(), gs.end(),
                                [&](const auto& g) { return g.card == intent.card; }), gs.end());
    }


    // CR 355: a spell's targets are chosen before its cost is determined and
    // paid, so effects that depend on what it chooses apply to the payment
    // (Sandswept Tomb, Irelia, Graceful). Spells that take one target through
    // Card::pickTarget choose it here; the rest keep the targets they were
    // played with.
    std::vector<GameObjectId> play_targets = intent.targets;
    bool target_chosen_at_play = false;
    if (play_targets.empty() && card.card_def_id != kInvalidId) {
        const Card* sc = card_registry_.get(card.card_def_id);
        if (sc && sc->needsPlayTimeTarget() && !sc->ignoresDeflect()) {
            const auto legal = sc->enumerateLegalTargets(state_, intent.player);
            if (!legal.empty()) {
                std::vector<Intent> options;
                options.reserve(legal.size());
                for (auto t : legal) {
                    Intent i;
                    i.type = IntentType::MakeChoice;
                    i.player = intent.player;
                    i.chosen_objects = {t};
                    options.push_back(std::move(i));
                }
                const Intent chosen = decideNow(intent.player, options,
                                                "choose a target for " + card.name);
                GameObjectId t = legal.front();
                if (!chosen.chosen_objects.empty() &&
                    std::find(legal.begin(), legal.end(), chosen.chosen_objects.front()) !=
                        legal.end())
                    t = chosen.chosen_objects.front();
                play_targets = {t};
                target_chosen_at_play = true;
                events_.logTrace("TARGET (play): " + card.name + " -> " +
                                 state_.getObject(t).name);
            }
        }
    }

    // Pay cost. A trash-replay grant overrides the printed cost (and its own
    // additional costs); otherwise the normal play_source-aware path runs.
    bool paid_via_grant = false;
    if (intent.play_source == Intent::PlaySource::Trash) {
        paid_via_grant = payTrashReplayGrant(intent.player, intent.card);
    }
    if (!paid_via_grant) {
        ps.current_play_source = intent.play_source;
        // Irelia, Graceful (462): "your spells that choose me cost [1]/[A] less."
        // Stage the largest per-target reduction among this spell's chosen
        // targets; canAfford/beginCostPayment subtract it.
        int tgt_disc = 0;
        for (auto t : play_targets)
            if (state_.objectExists(t))
                tgt_disc = std::max(tgt_disc,
                    state_.getObject(t).spells_targeting_me_cost_reduction);
        // Sandswept Tomb: "Each spell that chooses one or more units here that
        // are friendly to it costs [A] less."
        int tgt_power_disc = 0;
        for (auto t : play_targets) {
            if (!state_.objectExists(t)) continue;
            const auto& tobj = state_.getObject(t);
            if (!tobj.isUnit() || tobj.controller != intent.player) continue;
            if (auto tbf = tobj.battlefieldId())
                for (const auto& b : state_.battlefields)
                    if (b.id == *tbf)
                        tgt_power_disc = std::max(tgt_power_disc, b.friendly_spell_discount_power);
        }
        // "As an additional cost to play a spell, you may exhaust me and three
        // friendly units to reduce its cost by [2]." (Starry-Eyed Songstress)
        const int opt_disc = maybePaySpellCostOption(
            intent.player, intent.card, !canAfford(intent.player, intent.card, 0, tgt_disc));
        ps.transient_play_discount = tgt_disc + opt_disc;
        ps.transient_power_discount = tgt_power_disc;
        payCardCost(intent.player, intent.card);
        ps.transient_play_discount = 0;
        ps.transient_power_discount = 0;
        ps.current_play_source = Intent::PlaySource::Hand;

        // "You may pay X as an additional cost to play me" (spell variant).
        maybePayOptionalAdditionalCost(intent.player, intent.card);
    }

    // Repeat (CR 820) — agent-driven, modeled on Accelerate. After
    // paying the base cost we poll the agent yes/no for each
    // additional tranche they can still afford. Each "yes" pays an
    // extra [N][D] and bumps repeats_paid. Stops when the agent
    // declines or affordability runs out. Replaces the prior model
    // where the action generator pre-encoded one Play intent per
    // repeat count — that produced visually-identical legal-action
    // entries and forced the agent to commit to N before seeing how
    // the spell would resolve.
    int repeats = 0;
    RepeatCost repeat_cost{};
    if (card.card_def_id != kInvalidId) {
        repeat_cost = parseRepeatCost(card_db_.get(card.card_def_id).ability_text);
    }
    // GRANTED [Repeat] (when the spell has none of its own printed):
    //   The Academy (772): "next spell gets [Repeat] = its base cost" (one-shot).
    //   Syndra, Transcendent (708): "your spells have [Repeat] [2][P]" (continuous
    //   while she's in a showdown). Academy's one-shot grant takes precedence.
    if (card.isSpell() && !repeat_cost.valid) {
        if (ps.grant_repeat_base_to_next_spell && card.card_def_id != kInvalidId) {
            repeat_cost.energy = card_db_.get(card.card_def_id).energy_cost;
            repeat_cost.power = 0;
            repeat_cost.power_domain = Domain::Count;  // universal [A]
            repeat_cost.valid = repeat_cost.energy > 0;
            ps.grant_repeat_base_to_next_spell = false;  // consume
        } else if (ps.spells_have_repeat_energy > 0 || ps.spells_have_repeat_power > 0) {
            repeat_cost.energy = ps.spells_have_repeat_energy;
            repeat_cost.power = ps.spells_have_repeat_power;
            repeat_cost.power_domain = ps.spells_have_repeat_domain;
            repeat_cost.valid = true;
        }
    }
    // Marai Spire (525): "friendly [Repeat] costs cost [1] less."
    if (repeat_cost.valid && ps.repeat_cost_reduction > 0) {
        repeat_cost.energy = std::max(0, repeat_cost.energy - ps.repeat_cost_reduction);
    }
    if (repeat_cost.valid) {
        // CR 820.1.c.3: each Repeat cost can be paid only a single time (the spell has one).
        constexpr int kRepeatInstances = 1;
        while (repeats < kRepeatInstances &&
               canPayAdditionalCost(intent.player,
                                     repeat_cost.energy,
                                     repeat_cost.power,
                                     repeat_cost.power_domain,
                                     state_.purposePlaying(card.card_type))) {
            Intent decline;
            decline.type = IntentType::MakeChoice;
            decline.player = intent.player;
            decline.chosen_value = 0;
            Intent accept;
            accept.type = IntentType::MakeChoice;
            accept.player = intent.player;
            accept.chosen_value = 1;
            std::vector<Intent> opts = {decline, accept};

            state_.decision_index++;
            events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                             " (" + toString(intent.player) + "): "
                             "pay Repeat tranche " + std::to_string(repeats + 1) +
                             " for " + card.name + "? [decline|pay] [2 options]");
            auto chosen = getAgent(intent.player).selectAction(state_, opts);
            recordAppliedIntent(chosen);
            if (on_decision) on_decision(state_, opts, chosen);
            bool wants_pay = chosen.chosen_value.value_or(0) == 1;
            if (!wants_pay) break;
            if (!payRepeatCost(intent.player, repeat_cost,
                               state_.purposePlaying(card.card_type))) break;
            ++repeats;
        }
        if (repeats > 0) {
            events_.logTrace("REPEAT: paid " + std::to_string(repeats) +
                              " extra tranche(s) for " + card.name);
        }
    }

    // Track play count
    ps.cards_played_this_turn++;
    int energy_spent = 0;
    if (card.card_def_id != kInvalidId) {
        energy_spent = card_db_.get(card.card_def_id).energy_cost;
    }
    int total_energy_spent = energy_spent + repeats * repeat_cost.energy;
    // Snapshot on PlayerState so legend triggers can read it. Forgotten
    // Library / Virtuoso gate on the TOTAL energy spent (base + repeats).
    ps.last_spell_energy_spent = total_energy_spent;
    // Track the most-expensive single spell paid for this turn (Jhin,
    // Meticulous Killer: "if you've spent [4]+ to play a spell this turn").
    if (card.isSpell()) {
        ps.max_spell_spent_this_turn =
            std::max(ps.max_spell_spent_this_turn, total_energy_spent);
        // Bind a pending "next spell deals N Bonus Damage" rider (Ravenborn Tome)
        // onto this spell object so it applies to ALL instances it deals.
        if (ps.next_spell_bonus_damage > 0) {
            card.spell_bonus_damage = ps.next_spell_bonus_damage;
            ps.next_spell_bonus_damage = 0;
        }
    }
    events_.emit(CardPlayedEvent{intent.card, intent.player,
        card.card_type, ps.cards_played_this_turn, total_energy_spent});
    if (intent.play_source != Intent::PlaySource::Hand)
        events_.emit(ObjectStateChangedEvent{intent.card, "played_not_from_hand"});

    // Add spell to chain with targets. Carry energy_spent and repeats_paid
    // onto the chain item — ChainManager re-resolves the spell `repeats_paid`
    // extra times in its post-resume loop.
    auto chain_id = chain_manager_->addSpell(intent.card, intent.player, play_targets);
    (void)chain_id;
    if (!state_.chain.items.empty()) {
        auto& added = state_.chain.items.back();
        added.total_energy_spent = total_energy_spent;
        added.repeats_paid = repeats;
        if (target_chosen_at_play) {
            // Read by Card::pickTarget: the target is already chosen.
            while (added.resume_data.size() < 6) added.resume_data.push_back(0);
            added.resume_data[5] = 1;
        }
    }

    // Run the FEPR loop
    runChain();
}

} // namespace riftbound
