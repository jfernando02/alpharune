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

void GameEngine::runChain(bool resume) {
    // A chain run while another is resolving has no step resumeChain could
    // finish, so its priority decisions are not resume points.
    const ChainStep outer_step = state_.turn.chain_step;
    if (chain_depth_ > 0) state_.turn.chain_step = ChainStep::None;
    ++chain_depth_;
    struct DepthGuard {
        GameEngine& e;
        ChainStep step;
        ~DepthGuard() {
            if (--e.chain_depth_ > 0) e.state_.turn.chain_step = step;
        }
    } guard{*this, outer_step};

    chain_manager_->processFEPR(
        // Agent query callback
        [this](PlayerId player, const std::vector<Intent>& actions) -> Intent {
            return queryAgentForChain(player, actions);
        },
        // Permanent resolution callback (CR 337.1.c)
        [this](const ChainItem& item) {
            resolvePermanent(item);
        },
        // Spell resolution callback
        [this](const ChainItem& item) {
            resolveSpell(item);
        },
        // Closed State action generator (handles targeting + affordability)
        [this](PlayerId player) -> std::vector<Intent> {
            return generateClosedStateActions(player);
        },
        resume
    );
}

void GameEngine::resolveSpell(const ChainItem& item) {
    {
        std::string name = state_.objectExists(item.source) ? state_.getObject(item.source).name : "?";
        events_.logTrace(std::string(item.finalizing ? "FINALIZE-CHOICES: " : "RESOLVE: ") +
                         name + " (chain_item=" + std::to_string(item.id) +
                         ", ability=" + std::to_string(item.is_ability) +
                         ", spell=" + std::to_string(item.is_spell) + ")");
    }

    // Per-target legality re-validation at resolution time.
    //
    // CR 359.3.e.5: "If any of the spell's targets are no longer legal
    // when the spell resolves, those targets are unaffected by the
    // spell as it resolves" (per-target, not all-or-nothing).
    //
    // Phase 6q+ engine-audit CRITICAL #6 fix: pre-2026-05-19 the
    // engine fizzled the ENTIRE spell if any single target was
    // illegal at resolve. Challenge / Star-Crossed / Deathgrip
    // silently no-op'd whenever one of their two targets died,
    // bounced, or otherwise left the legal-target set between play
    // time and resolve time. Per CR, the legal target(s) should
    // still receive their effect; only the illegal one(s) are
    // skipped. We implement this by FILTERING `item.targets` rather
    // than aborting — cards then read targets[i] and naturally skip
    // any that have already been removed from the legal set.
    //
    // We pass the filtered list into onResolve/onActivate/onTrigger
    // via a local copy. The chain item itself is left untouched
    // (avoids interfering with chain resume / multi-resolve paths).
    //
    // Cards that defer target choice to resolve time via
    // pickTarget/pickTargetPair (Phase 6q opt-in) are unaffected —
    // they re-enumerate legal targets at resolve time themselves.
    Card* card = card_registry_.get(item.card_def_id);
    std::vector<GameObjectId> effective_targets = item.targets;
    if (card && !item.targets.empty()) {
        // An activated ability is re-validated against ITS OWN requirements
        // and legal set (multi-ability cards differ per ability).
        auto reqs = card->getTargetRequirements();
        auto legal = (item.is_ability && !item.is_activated_ability)
            ? card->enumerateLegalTargets(state_, item.controller,
                                          kTriggerAbilityIndex, item.source)
            : card->enumerateLegalTargets(state_, item.controller);
        if (item.is_ability && item.is_activated_ability) {
            const Card* acard = (item.granted_ability_def != 0)
                ? card_registry_.get(item.granted_ability_def) : card;
            if (acard) {
                auto abs = acard->activatedAbilities();
                if (item.ability_index >= 0 &&
                    item.ability_index < static_cast<int>(abs.size())) {
                    reqs = abs[item.ability_index].targets;
                    legal = acard->enumerateLegalTargets(state_, item.controller,
                                                         item.ability_index, item.source);
                }
            }
        }
        if (reqs.count > 0) {
            std::vector<GameObjectId> filtered;
            filtered.reserve(item.targets.size());
            int dropped = 0;
            for (auto t : item.targets) {
                if (std::find(legal.begin(), legal.end(), t) != legal.end()) {
                    filtered.push_back(t);
                } else {
                    ++dropped;
                }
            }
            if (dropped > 0) {
                std::string name = state_.objectExists(item.source)
                    ? state_.getObject(item.source).name : "?";
                events_.logTrace("RESOLVE: " + name + " — " +
                                  std::to_string(dropped) +
                                  " target(s) no longer legal "
                                  "(per-CR 359.3.e.5: unaffected, "
                                  "rest of spell still resolves)");
            }
            effective_targets = std::move(filtered);
        }
    }

    // Dispatch through Card object
    CardContext ctx{state_, events_, *effect_executor_,
                    item.controller, item.source};
    ctx.firing_trigger = item.fired_trigger;
    ctx.registry = &card_registry_;  // lets effects dispatch into other cards' logic

    if (card) {
        if (item.is_ability) {
            // Activated abilities ([E]:, [N]:, etc. — player chose to
            // pay cost + activate) dispatch through onActivate; triggered
            // abilities (event-driven, e.g. WhenIPlay, AtEndOfTurn)
            // dispatch through onTrigger. The two have different
            // semantics so cards override the matching method only —
            // the other defaults to a no-op. The is_activated_ability
            // flag is set on the chain item by ChainManager::addAbility.
            if (item.is_activated_ability) {
                // Phase 6r — dispatch via the multi-ability overload.
                // Default Card impl falls through to onActivate(ctx, targets)
                // so single-ability cards keep working unchanged.
                // Aura-granted ability (Forge/Gardens/Heimer): run the GRANTING
                // card's onActivate with the bearer (item.source) as ctx.source.
                Card* act_card = (item.granted_ability_def != 0)
                    ? card_registry_.get(item.granted_ability_def) : card;
                bool is_empower = false;
                if (act_card) {
                    auto abs = act_card->activatedAbilities();
                    is_empower = item.ability_index >= 0 &&
                        item.ability_index < static_cast<int>(abs.size()) &&
                        abs[item.ability_index].is_empower;
                }
                if (is_empower) {
                    // [Empower] (CR 827.2): the source becomes Empowered.
                    effect_executor_->empower(item.source);
                } else if (act_card) {
                    act_card->onActivate(ctx, item.ability_index, effective_targets);
                }
                // Prize of Progress: a gear's activated ability was used.
                if (!item.finalizing && state_.objectExists(item.source) &&
                    state_.getObject(item.source).isGear()) {
                    events_.emit(ObjectStateChangedEvent{item.source,
                                                         "gear_ability_used"});
                }
            } else {
                card->onTrigger(ctx, effective_targets);
            }
        } else {
            // Spell resolution
            card->onResolve(ctx, effective_targets);
        }
    }
}

void GameEngine::resolvePermanent(const ChainItem& item) {
    // CR 337.1.c / CR 359.2: Permanent leaves chain, becomes game object on board.
    // Location was pre-selected during executePlayCard and stored on the object.
    auto& card = state_.getObject(item.source);
    auto loc = card.location.value_or(BaseLocation{item.controller});

    card.zone = ZoneType::Base;
    card.location = loc;

    // Units enter exhausted (CR 143.4) unless Accelerate / an "enters ready" effect
    if (card.isUnit()) {
        bool enters_ready = false;
        // Accelerate (CR 805.1.a): "You may pay [1] and 1 Power as an
        // additional cost. If you do, I enter ready."
        //   • CR 805.1.a.1 — single-domain unit: Power must match the
        //     unit's domain.
        //   • CR 805.1.a.2 — multi-domain or no-domain unit: Power may be
        //     any domain (i.e., [A]).
        // The "you may" makes this OPTIONAL — surfaced as a yes/no
        // decision to the agent. (Previously auto-applied whenever
        // affordable, which silently spent the player's energy + rune
        // every time the Accelerate cost happened to be payable.)
        const bool granted_accelerate = card.card_counters.count("__granted_accelerate") &&
                                        card.card_counters["__granted_accelerate"] != 0;
        if (granted_accelerate) card.card_counters["__granted_accelerate"] = 0;
        if (card.keywords.has(Keyword::Accelerate) || granted_accelerate) {
            // CR 805.1.b.1: the Power must match one of the unit's own domains;
            // 805.1.b.2: a unit with no domain pays it with [A].
            std::optional<Domain> domain_to_pay;
            if (card.domains.empty()) {
                if (canPayAdditionalCost(item.controller, 1, 1, Domain::Count,
                                         state_.purposePlaying(CardType::Unit)))
                    domain_to_pay = Domain::Count;
            } else {
                for (Domain d : card.domains) {
                    if (canPayAdditionalCost(item.controller, 1, 1, d,
                                             state_.purposePlaying(CardType::Unit))) {
                        domain_to_pay = d;
                        break;
                    }
                }
            }
            if (domain_to_pay.has_value()) {
                Intent decline;
                decline.type = IntentType::MakeChoice;
                decline.player = item.controller;
                decline.chosen_value = 0;  // 0 = decline
                Intent accept;
                accept.type = IntentType::MakeChoice;
                accept.player = item.controller;
                accept.chosen_value = 1;   // 1 = pay [1][D] for Accelerate
                std::vector<Intent> opts = {decline, accept};

                state_.decision_index++;
                events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                                 " (" + toString(item.controller) + "): "
                                 "pay [1][" + (*domain_to_pay == Domain::Count ? "A"
                                     : toString(*domain_to_pay)) +
                                 "] for Accelerate on " + card.name +
                                 "? [decline|pay] [2 options]");

                auto chosen = getAgent(item.controller).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);

                bool wants_pay = chosen.chosen_value.value_or(0) == 1;
                if (wants_pay) {
                    events_.logTrace("ACCELERATE: " + card.name + " pays [1] + power to enter ready");
                    payAdditionalCost(item.controller, 1, 1, *domain_to_pay,
                                      state_.purposePlaying(CardType::Unit));
                    enters_ready = true;
                } else {
                    events_.logTrace("ACCELERATE: " + card.name + " declined");
                }
            }
        }
        // [Ambush] (CR 822.1.b) grants only a play location and [Reaction]
        // timing ("I have [Reaction] as long as I'm being played to a
        // battlefield where you control Units"). It does NOT make the unit
        // enter ready — CR 143.4 still applies.
        // Cards that intrinsically enter ready (e.g. Daisy! "I enter ready").
        if (card.card_def_id != kInvalidId) {
            if (const Card* cdef = card_registry_.get(card.card_def_id)) {
                // Prefer the state-aware overload so cards with
                // conditional ready (Monch) can consult state. Default
                // impl falls through to the no-arg version.
                if (cdef->entersReadyOnPlay(state_, card.controller, card)) enters_ready = true;
            }
        }
        // "Units you play this turn enter ready." (Confront)
        if (state_.player(card.controller).units_enter_ready_this_turn) enters_ready = true;
        // "Other units enter ready." (Firebrand Raider)
        if (state_.turn.all_units_enter_ready) enters_ready = true;
        // Another card of the controller's (on the board or in the Legend Zone)
        // makes it enter ready (Card::grantsEnterReadyTo).
        if (!enters_ready) {
            for (const auto& [oid, obj] : state_.objects) {
                if (obj.controller != card.controller || obj.card_def_id == kInvalidId) continue;
                if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
                const Card* granter = card_registry_.get(obj.card_def_id);
                if (granter && granter->grantsEnterReadyTo(state_, oid, card)) {
                    enters_ready = true;
                    break;
                }
            }
        }
        card.is_exhausted = !enters_ready;
    }
    // Gear enters ready (CR 149.1) unless it says "This enters exhausted."
    if (card.isGear()) {
        const Card* gear_def = card.card_def_id != kInvalidId
            ? card_registry_.get(card.card_def_id) : nullptr;
        card.is_exhausted = gear_def && gear_def->entersExhausted();
    }

    card.recomputeMight();

    events_.emit(EnteredBoardEvent{item.source, item.controller,
        card.card_type, loc, true});

    // Quick-Draw: auto-attach to target unit if specified
    if (card.isGear() && card.hasKeyword(Keyword::QuickDraw) && !item.targets.empty()) {
        auto target_unit = item.targets[0];
        if (state_.objectExists(target_unit)) {
            events_.logTrace("QUICK_DRAW: auto-attaching " + card.name + " to " +
                             state_.getObject(target_unit).name);
            attachGearToUnit(item.source, target_unit);
        }
    }

    // Check if playing to a battlefield contests it
    if (card.isUnit() && std::holds_alternative<BattlefieldLocation>(loc)) {
        auto bf_id = std::get<BattlefieldLocation>(loc).id;
        auto& bf = getBattlefield(bf_id);

        if (!bf.controller.has_value() || *bf.controller != item.controller) {
            if (!bf.is_contested) {
                bf.is_contested = true;
                bf.contested_by = item.controller;
                events_.emit(ContestedEvent{bf_id, item.controller});
            }
            // Re-evaluate staging on every arrival at a contested BF.
            // See executeStandardMove for the rationale (showdowns that
            // end with 0 units leave the BF stuck without this).
            if (!bf.showdown_staged && !bf.combat_staged &&
                !bf.showdown_in_progress && !bf.combat_in_progress) {
                bool has_opponent = bf.hasUnitsFrom(
                    opponent(item.controller), state_.objects);
                if (has_opponent) {
                    bf.combat_staged = true;
                } else {
                    bf.showdown_staged = true;
                }
            }
        }
    }
}

Intent GameEngine::queryAgentForChain(PlayerId player,
                                       const std::vector<Intent>& actions) {
    state_.decision_index++;
    auto chosen = getAgent(player).selectAction(state_, actions);
    recordAppliedIntent(chosen);
    // Surface the chain-priority decision in the trace so V&V can
    // confirm priority is being passed correctly between players in
    // Closed State (CR 337). Without this, trivial-pass decisions are
    // invisible — the renderer suppresses the panel for single-option
    // decisions, leaving no marker at all that the engine offered
    // priority to player N. Format mirrors the main DECISION trace lines.
    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                     " (" + std::string(toString(player)) + " — chain priority): " +
                     std::to_string(actions.size()) + " legal actions");
    events_.logTrace("CHOSE: [" + std::string(toString(player)) + "] " +
                     std::string(toString(chosen.type)));
    if (on_decision) {
        on_decision(state_, actions, chosen);
    }
    return chosen;
}

bool GameEngine::canStandardMoveTo(const GameObject& unit, const LocationId& dest) const {
    if (!unit.isUnit() || !unit.location.has_value() || unit.is_exhausted) return false;
    if (unit.cant_move_on_turn == state_.turn.turn_number) return false;
    if (unit.isAtBase()) return std::holds_alternative<BattlefieldLocation>(dest);
    const auto from = unit.battlefieldId();
    if (!from) return false;
    if (const auto* to_base = std::get_if<BaseLocation>(&dest)) {
        if (to_base->player != unit.controller || unit.cant_move_to_base) return false;
        return !getBattlefield(*from).blocks_move_to_base &&
               !getBattlefield(*from).aura_blocks_move_to_base;
    }
    const auto to = std::get<BattlefieldLocation>(dest).id;
    if (to == *from) return false;
    return unit.hasKeyword(Keyword::Ganking) || getBattlefield(to).accepts_any_inbound;
}

std::pair<int, int> GameEngine::moveCost(PlayerId player, const LocationId& dest,
                                         int n_units) const {
    const auto* at = std::get_if<BattlefieldLocation>(&dest);
    if (!at || n_units <= 0) return {0, 0};
    const auto& bf = getBattlefield(at->id);
    const int pi = playerIndex(player);
    return {bf.move_tax_energy[pi],
            bf.move_tax_power[pi] + bf.multi_move_surcharge_power[pi] * (n_units - 1)};
}

bool GameEngine::canPayAnyDomainCost(PlayerId player, int energy, int power) const {
    if (energy <= 0 && power <= 0) return true;
    for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
        if (canPayAdditionalCost(player, energy, power, static_cast<Domain>(di))) return true;
    return false;
}

void GameEngine::payAnyDomainCost(PlayerId player, int energy, int power) {
    if (energy <= 0 && power <= 0) return;
    for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
        if (canPayAdditionalCost(player, energy, power, static_cast<Domain>(di))) {
            payAdditionalCost(player, energy, power, static_cast<Domain>(di));
            return;
        }
}

void GameEngine::executeStandardMove(const Intent& intent) {
    if (!intent.move_destination.has_value()) return;
    auto dest = *intent.move_destination;

    // CR 144.3: other units may make their standard move at the same time,
    // to the same destination (origins may differ). Offered one at a time
    // until the player is done; each must keep the move's cost payable.
    std::vector<GameObjectId> movers = intent.units_to_move;
    for (;;) {
        std::vector<GameObjectId> more;
        const auto cost = moveCost(intent.player, dest, static_cast<int>(movers.size()) + 1);
        if (!canPayAnyDomainCost(intent.player, cost.first, cost.second)) break;
        for (const auto& [id, o] : state_.objects) {
            if (o.controller != intent.player) continue;
            if (std::find(movers.begin(), movers.end(), id) != movers.end()) continue;
            if (canStandardMoveTo(o, dest)) more.push_back(id);
        }
        if (more.empty()) break;
        std::vector<Intent> opts;
        Intent done; done.type = IntentType::MakeChoice; done.player = intent.player;
        done.chosen_value = 0;
        opts.push_back(done);
        for (auto id : more) {
            Intent add; add.type = IntentType::MakeChoice; add.player = intent.player;
            add.chosen_objects = {id};
            opts.push_back(add);
        }
        state_.decision_index++;
        events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                         toString(intent.player) + "): move another unit along? [" +
                         std::to_string(opts.size()) + " options]");
        auto chosen = getAgent(intent.player).selectAction(state_, opts);
        recordAppliedIntent(chosen);
        if (on_decision) on_decision(state_, opts, chosen);
        if (chosen.chosen_objects.empty() ||
            std::find(more.begin(), more.end(), chosen.chosen_objects[0]) == more.end())
            break;
        movers.push_back(chosen.chosen_objects[0]);
    }
    // A cost to move units here (Gravity Field; Mageseeker Investigator's [A]
    // per extra unit), paid once for the whole move.
    {
        const auto cost = moveCost(intent.player, dest, static_cast<int>(movers.size()));
        if (cost.first > 0 || cost.second > 0) {
            payAnyDomainCost(intent.player, cost.first, cost.second);
            events_.logTrace("MOVE_COST: paid " + std::to_string(cost.first) + " energy + " +
                             std::to_string(cost.second) + " power");
        }
    }

    for (auto unit_id : movers) {
        auto& unit_obj = state_.getObject(unit_id);
        std::string dest_str = std::holds_alternative<BattlefieldLocation>(dest)
            ? "BF#" + std::to_string(std::get<BattlefieldLocation>(dest).id)
            : "Base(" + std::string(toString(std::get<BaseLocation>(dest).player)) + ")";
        events_.logTrace("MOVE: " + unit_obj.name + " (id=" + std::to_string(unit_id) +
                         ") -> " + dest_str);
        auto& unit = state_.getObject(unit_id);

        // Pay cost: exhaust (CR 144.2)
        unit.is_exhausted = true;
        events_.emit(ObjectStateChangedEvent{unit_id, "exhausted"});

        auto old_location = unit.location;
        moveUnit(unit_id, dest);

        events_.emit(UnitMovedEvent{
            unit_id, intent.player,
            old_location.value_or(BaseLocation{intent.player}),
            dest, true
        });
    }

    // Check if this contests a battlefield
    if (std::holds_alternative<BattlefieldLocation>(dest)) {
        auto bf_id = std::get<BattlefieldLocation>(dest).id;
        auto& bf = getBattlefield(bf_id);

        // CR 459.2.b.3.a / .4.a — units arriving during ongoing combat
        // inherit the appropriate combat designation. Phase 6q+ fix:
        // previously the unit sat with combat_designation = None and
        // didn't contribute to damage assignment / step resolution.
        if (bf.combat_in_progress) {
            for (auto unit_id : movers) {

                auto& unit_obj = state_.getObject(unit_id);
                if (bf.attacker.has_value() && unit_obj.controller == *bf.attacker) {
                    unit_obj.combat_designation = CombatDesignation::Attacker;
                } else if (bf.defender.has_value() && unit_obj.controller == *bf.defender) {
                    unit_obj.combat_designation = CombatDesignation::Defender;
                }
                unit_obj.recomputeMight();
            }
        }

        // Contested if moving to a BF we don't control (CR 187.3.a)
        if (!bf.controller.has_value() || *bf.controller != intent.player) {
            if (!bf.is_contested) {
                bf.is_contested = true;
                bf.contested_by = intent.player;
                events_.emit(ContestedEvent{bf_id, intent.player});
            }
            // Always re-evaluate staging when a unit arrives at a
            // contested BF. Previously this block only fired on the
            // FIRST contest, which left BFs permanently stuck when a
            // showdown ended with 0 units on both sides (sole_player =
            // None → is_contested stays true → subsequent moves saw
            // `is_contested == true` and skipped staging → combat
            // never fired → no control ever established → 0-0 draws.
            if (!bf.showdown_staged && !bf.combat_staged &&
                !bf.showdown_in_progress && !bf.combat_in_progress) {
                bool has_opponent_units = bf.hasUnitsFrom(
                    opponent(intent.player), state_.objects);
                if (has_opponent_units) {
                    bf.combat_staged = true;
                } else {
                    bf.showdown_staged = true;
                }
            }
        }
    }
}

void GameEngine::executeEndTurn(const Intent&) {
    // Nothing to do — main phase loop handles this
}

void GameEngine::executeMulligan(const Intent& intent) {
    auto& ps = state_.player(intent.player);

    if (intent.cards_to_mulligan.empty()) return;

    // Set aside chosen cards (CR 118.1)
    std::vector<GameObjectId> set_aside;
    for (auto card_id : intent.cards_to_mulligan) {
        auto it = std::find(ps.hand.begin(), ps.hand.end(), card_id);
        if (it != ps.hand.end()) {
            set_aside.push_back(*it);
            ps.hand.erase(it);
            events_.logTrace("  MULLIGAN: " + std::string(toString(intent.player)) +
                             " set aside " + state_.getObject(card_id).name +
                             " (id=" + std::to_string(card_id) + ")");
        }
    }

    // Draw replacements (CR 118.2). Snapshot hand size so we can emit
    // explicit "MULLIGAN: drew X" lines for the replacements (the
    // executor's generic DREW: lines also fire, but the spell-prefixed
    // form makes the connection scan-able in the trace).
    size_t before_draw = ps.hand.size();
    drawCards(intent.player, static_cast<int>(set_aside.size()));
    for (size_t i = before_draw; i < ps.hand.size(); ++i) {
        auto did = ps.hand[i];
        events_.logTrace("  MULLIGAN: " + std::string(toString(intent.player)) +
                         " drew replacement " + state_.getObject(did).name +
                         " (id=" + std::to_string(did) +
                         ") — PRIVATE to " + toString(intent.player));
    }

    // Recycle set-aside cards to bottom of deck in random order (CR 118.3)
    std::shuffle(set_aside.begin(), set_aside.end(), rng_);
    for (auto card_id : set_aside) {
        state_.getObject(card_id).zone = ZoneType::MainDeck;
        ps.main_deck.insert(ps.main_deck.begin(), card_id); // bottom = front
    }
}

void GameEngine::executePassFocus(const Intent& intent) {
    state_.turn.players_passed_focus.insert(intent.player);
}

void GameEngine::executeHideCard(const Intent& intent) {
    auto& ps = state_.player(intent.player);
    auto& card = state_.getObject(intent.card);

    events_.logTrace("HIDE: " + card.name + " at BF#" +
                     std::to_string(intent.chosen_battlefield) +
                     " by " + toString(intent.player));

    // Remove from hand
    auto it = std::find(ps.hand.begin(), ps.hand.end(), intent.card);
    if (it != ps.hand.end()) ps.hand.erase(it);

    // Pay [A] cost — recycle a ready rune (universal power cost), unless
    // the player may hide ignoring costs this turn, or pays [1] instead
    // (Swift Scout: the player's choice when both can be paid).
    auto base_loc = BaseLocation{intent.player};
    bool pay_energy = false;
    if (!ps.hide_ignoring_costs_this_turn && ps.hide_for_energy &&
        canPayAdditionalCost(intent.player, 1, 0, Domain::Fury, SpendPurpose{})) {
        bool rune_ready = false;
        for (const auto& [id, obj] : state_.objects)
            if (obj.isRune() && obj.controller == intent.player && !obj.is_exhausted &&
                obj.location.has_value() && *obj.location == LocationId{base_loc}) { rune_ready = true; break; }
        pay_energy = true;
        if (rune_ready) {
            Intent a; a.type = IntentType::MakeChoice; a.player = intent.player; a.chosen_value = 0;
            Intent e; e.type = IntentType::MakeChoice; e.player = intent.player; e.chosen_value = 1;
            std::vector<Intent> opts = {a, e};
            state_.decision_index++;
            events_.logTrace("DECISION #" + std::to_string(state_.decision_index) + " (" +
                             toString(intent.player) + "): hide for [A] or [1]?");
            auto chosen = getAgent(intent.player).selectAction(state_, opts);
            recordAppliedIntent(chosen);
            if (on_decision) on_decision(state_, opts, chosen);
            pay_energy = chosen.chosen_value.value_or(0) == 1;
        }
        if (pay_energy) payAdditionalCost(intent.player, 1, 0, Domain::Fury, SpendPurpose{});
    }
    for (auto& [id, obj] : state_.objects) {
        if (ps.hide_ignoring_costs_this_turn || pay_energy) break;
        if (!obj.isRune() || obj.controller != intent.player || obj.is_exhausted) continue;
        if (!obj.location.has_value() || *obj.location != LocationId{base_loc}) continue;
        // Recycle the rune for [A]
        events_.logTrace("HIDE_COST: recycled " + obj.name + " (id=" +
                         std::to_string(id) + ") to pay [A]");
        obj.location = std::nullopt;
        obj.zone = ZoneType::RuneDeck;
        ps.rune_deck.insert(ps.rune_deck.begin(), id);
        events_.emit(LeftBoardEvent{id, intent.player, CardType::Rune,
            base_loc, ZoneType::RuneDeck, false});
        break;
    }

    // Place facedown at the chosen battlefield
    auto bf_id = intent.chosen_battlefield;
    auto& bf = getBattlefield(bf_id);
    bf.facedown.push_back(intent.card);

    card.zone = ZoneType::FacedownZone;
    card.location = std::nullopt; // facedown zones are not locations per rules
    card.is_hidden = true;
    card.hidden_at = bf_id;
    card.hidden_on_turn = state_.turn.turn_number;

    events_.logDebug(std::string("HIDDEN: ") + card.name + " hidden at BF#" +
                     std::to_string(bf_id) + " by " + toString(intent.player));

    // "When you hide a card" (Katarina, Reckless).
    events_.emit(CardHiddenEvent{intent.card, intent.player});
}

void GameEngine::executePlayFromHidden(const Intent& intent) {
    // A unit that may be played to any battlefield (Last Caress) carries its
    // chosen battlefield.
    std::optional<BattlefieldId> to_bf;
    if (intent.play_location.has_value())
        if (const auto* at = std::get_if<BattlefieldLocation>(&*intent.play_location))
            to_bf = at->id;
    playFromFacedown(intent.player, intent.card, intent.targets, to_bf);
    runChain();
}

// CR 811: a card played from facedown is played ignoring its base cost; a permanent goes to
// the battlefield it was hidden at (811.1.d.1), and the choices of a spell, or of a play
// effect of a permanent, are made among options at that battlefield (811.1.d.2-3). The card
// carries "__hidden_play_bf" while its chain items are created, so they record it
// (ChainItem::hidden_at).
void GameEngine::playFromFacedown(PlayerId player, GameObjectId card_id,
                                  const std::vector<GameObjectId>& targets,
                                  std::optional<BattlefieldId> to_bf) {
    auto& card = state_.getObject(card_id);
    const BattlefieldId bf_id = card.hidden_at;
    for (auto& bf : state_.battlefields) {
        auto it = std::find(bf.facedown.begin(), bf.facedown.end(), card_id);
        if (it != bf.facedown.end()) bf.facedown.erase(it);
    }
    card.is_hidden = false;
    card.hidden_at = kInvalidId;
    // Lets the card's own "When you play me from face down" tell this play
    // apart from a play from hand. Cleared when it next enters a hand/trash.
    card.card_counters["__played_from_facedown"] = 1;
    card.card_counters["__hidden_play_bf"] = static_cast<int>(bf_id) + 1;
    events_.logTrace("PLAY (from facedown): " + card.name + " at BF#" + std::to_string(bf_id));

    // "When you play a card from face down" (Katarina, Reckless).
    events_.emit(PlayedFromFacedownEvent{card_id, player});

    auto& ps = state_.player(player);
    ps.cards_played_this_turn++;
    if (card.isPermanent()) card.location = BattlefieldLocation{to_bf.value_or(bf_id)};
    if (card.isUnit()) ps.next_unit_any_bf = false;   // Last Caress: "the next unit"
    // Hidden play is "ignoring its base cost" (CR 811) — energy_spent = 0.
    events_.emit(CardPlayedEvent{card_id, player, card.card_type,
                                 ps.cards_played_this_turn, /*energy_spent=*/0});
    // Played from face down = from somewhere other than hand.
    events_.emit(ObjectStateChangedEvent{card_id, "played_not_from_hand"});
    if (state_.objectExists(card_id)) {
        auto& c = state_.getObject(card_id);
        if (c.isSpell()) chain_manager_->addSpell(card_id, player, targets);
        else chain_manager_->addPermanent(card_id, player);
        state_.getObject(card_id).card_counters.erase("__hidden_play_bf");
    }
}

} // namespace riftbound
