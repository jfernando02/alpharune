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

// ── Combat damage and replacement effects (CR 467.2.c.5) ─────────────────────
// "When assigning damage in this way, passive abilities and replacement effects
// that would apply to the resulting damage are considered to apply to the
// assignment instead." These helpers are the single place that says how an
// ASSIGNED amount becomes DEALT damage for a unit, so the assignment cascade
// and the application step always agree.
namespace {

/// Damage actually dealt when `assigned` combat damage is assigned to `t`.
int combatDamageDealt(const GameObject& t, int assigned) {
    if (assigned <= 0) return 0;
    if (t.immune_to_damage) return 0;                    // can't be dealt damage
    if (t.prevent_next_damage_this_turn) return 0;       // next instance prevented
    int dealt = std::max(0, assigned - t.prevent_damage_remaining);
    if (t.damage_doubled_this_turn) dealt *= 2;
    return dealt;
}

/// CR 467.2.c.10: "If a unit cannot be dealt damage, then no amount of damage
/// can be considered lethal. Such a unit is exempt from any considerations of
/// mandatory assignment."
bool canTakeCombatDamage(const GameObject& t) {
    return !t.immune_to_damage && !t.prevent_next_damage_this_turn;
}

/// Minimum ASSIGNED amount that deals lethal damage to `t` (CR 467.2.c.4.a).
/// `any_damage_lethal`: the assigning player's damage kills in any amount
/// (CR 142.4.c — Elder Dragon).
int combatDamageNeededForLethal(const GameObject& t, bool any_damage_lethal) {
    int lethal = any_damage_lethal ? 1 : std::max(1, t.current_might - t.damage_marked);
    if (t.damage_doubled_this_turn) lethal = (lethal + 1) / 2;
    return lethal + t.prevent_damage_remaining;
}

/// Greedy-lethal cascade over `order` (CR 467.2.c.3 / .4): each unit gets
/// exactly lethal before the next gets any; excess goes on the last unit that
/// can be damaged. Units that can't be dealt damage are skipped (467.2.c.10).
std::vector<DamageAssignment> lethalCascade(const GameState& state,
                                            const std::vector<GameObjectId>& order,
                                            int total_damage, bool any_damage_lethal) {
    std::vector<DamageAssignment> da;
    int remaining = total_damage;
    for (auto tid : order) {
        if (remaining <= 0) break;
        const auto& t = state.getObject(tid);
        if (!canTakeCombatDamage(t)) continue;
        int assigned = std::min(remaining, combatDamageNeededForLethal(t, any_damage_lethal));
        da.push_back({tid, assigned});
        remaining -= assigned;
    }
    if (remaining > 0) {
        // "unless no further units remain to have damage assigned to them"
        if (!da.empty()) da.back().damage += remaining;
        else if (!order.empty()) da.push_back({order.front(), remaining});  // nothing damageable
    }
    return da;
}

}  // namespace

bool GameEngine::anyDamageIsLethalFor(PlayerId damager) const {
    // Elder Dragon: "Any amount of your damage is enough to kill." (CR 142.4.c)
    for (const auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        if (obj.controller != damager || obj.card_def_id == kInvalidId) continue;
        const auto& text = card_db_.get(obj.card_def_id).ability_text;
        if (text.find("Any amount of your damage is enough to kill") != std::string::npos ||
            text.find("any amount of your damage is enough to kill") != std::string::npos)
            return true;
    }
    return false;
}

void GameEngine::applyCombatDamage(GameObjectId target_id, int assigned) {
    if (assigned <= 0 || !state_.objectExists(target_id)) return;
    auto& t = state_.getObject(target_id);
    // Damage assigned beyond what this unit needed for lethal is excess
    // ("if you assigned N or more excess damage").
    {
        const PlayerId assigner = opponent(t.controller);
        const int needed = canTakeCombatDamage(t)
            ? combatDamageNeededForLethal(t, anyDamageIsLethalFor(assigner)) : 0;
        if (assigned > needed) state_.player(assigner).excess_combat_damage += assigned - needed;
    }
    const int dealt = combatDamageDealt(t, assigned);
    // Consume the prevention that applied to this instance.
    if (t.prevent_next_damage_this_turn) {
        t.prevent_next_damage_this_turn = false;
    } else if (!t.immune_to_damage) {
        t.prevent_damage_remaining -= std::min(assigned, t.prevent_damage_remaining);
    }
    if (dealt != assigned)
        events_.logTrace("  COMBAT_DAMAGE: " + t.name + " assigned " + std::to_string(assigned) +
                         " -> dealt " + std::to_string(dealt));
    if (dealt <= 0) return;
    if (t.damage_marked == 0) t.damage_marked_by[0] = t.damage_marked_by[1] = 0;
    t.damage_marked += dealt;
    t.damage_taken_this_turn += dealt;
    t.damage_marked_by[playerIndex(opponent(t.controller))] += dealt;
    events_.emit(DamageDealtEvent{target_id, dealt, kInvalidId, true});
}

void GameEngine::executeAssignCombatDamage(const Intent& intent) {
    // Apply damage assignments
    for (auto& assignment : intent.damage_assignments) {
        applyCombatDamage(assignment.target_unit, assignment.damage);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Combat and Showdown
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::runShowdown(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);
    events_.logTrace("SHOWDOWN: BF#" + std::to_string(bf_id) +
                     (bf.combat_staged ? " (combat)" : " (non-combat)"));

    bf.showdown_in_progress = true;
    state_.turn.ns_state = NeutralShowdownState::Showdown;
    state_.turn.oc_state = openUnlessChain();   // CR 309.1

    events_.emit(ShowdownStartedEvent{bf_id, bf.combat_staged});

    // Showdown loop: players alternate focus, playing Action/Reaction spells.
    // Showdown closes when all players pass focus consecutively (CR 348.1).
    runShowdownLoop(bf_id);
    finishShowdown(bf_id);
}

void GameEngine::finishShowdown(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);
    bf.showdown_in_progress = false;
    state_.turn.ns_state = NeutralShowdownState::Neutral;
    state_.turn.oc_state = openUnlessChain();   // CR 309.1
    events_.emit(ShowdownEndedEvent{bf_id});

    // Non-combat showdown resolution: if only one player has units,
    // they establish control (CR 348.2.a)
    auto p1_units = state_.unitsAt(BattlefieldLocation{bf_id}, PlayerId::Player1);
    auto p2_units = state_.unitsAt(BattlefieldLocation{bf_id}, PlayerId::Player2);

    PlayerId sole_player = PlayerId::None;
    if (!p1_units.empty() && p2_units.empty()) sole_player = PlayerId::Player1;
    if (p1_units.empty() && !p2_units.empty()) sole_player = PlayerId::Player2;

    if (sole_player != PlayerId::None) {
        auto old_controller = bf.controller;
        bf.controller = sole_player;
        bf.is_contested = false;

        events_.emit(ControlChangedEvent{bf_id, old_controller, sole_player});
        bf.conquered_from_uncontrolled = !old_controller.has_value();

        // CR 468.5 / 471.1: a player Establishes Control (and so Conquers) only
        // "if they didn't already control this Battlefield".
        if (!old_controller.has_value() || *old_controller != sole_player)
            scoreConquer(sole_player, bf_id);
    }
}

// C-1 commit 7 — showdown loop split into advance/resolve halves.
// Behavior is identical to the prior monolithic body; the split lets
// the future step machine (commit 9) call the halves directly without
// going through queryAgent.

namespace {
constexpr int kMaxShowdownActions = 100; // safety cap
}  // namespace

GameEngine::ShowdownAdvance GameEngine::advanceShowdown(BattlefieldId bf_id,
                                                          PlayerId focus_holder,
                                                          int& action_count) {
    ShowdownAdvance adv;
    if (action_count >= kMaxShowdownActions || state_.game_over) {
        adv.kind = ShowdownAdvance::Kind::Done;
        return adv;
    }
    state_.turn.focus_holder = focus_holder;
    state_.turn.priority_holder = focus_holder;
    state_.turn.oc_state = openUnlessChain();   // CR 309.1

    events_.emit(PriorityGrantedEvent{focus_holder, true});

    adv.kind = ShowdownAdvance::Kind::NeedDecision;
    adv.focus_holder = focus_holder;
    adv.legal = generateShowdownActions(focus_holder);
    (void)bf_id;  // bf only matters for the caller's later steps
    return adv;
}

std::optional<PlayerId> GameEngine::resolveShowdownDecision(
    BattlefieldId bf_id, const Intent& chosen, PlayerId current_focus,
    int& action_count) {
    action_count++;

    if (chosen.type == IntentType::PassFocus) {
        state_.turn.players_passed_focus.insert(current_focus);

        // Check if all players passed focus
        bool all_passed = true;
        for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
            if (state_.turn.players_passed_focus.find(pid) ==
                state_.turn.players_passed_focus.end()) {
                all_passed = false;
                break;
            }
        }

        if (all_passed) return std::nullopt;  // showdown closes
        return opponent(current_focus);
    }
    // CR 806 + 813 + 819 + 822 — every action available during a
    // showdown should be routed through executeIntent. Pre-2026-05-19
    // engine-audit CRITICAL #3 fix: the switch only handled
    // PlayActionCard. Ambush units (PlayActionCard with play_location)
    // would route here correctly because they use PlayActionCard, BUT
    // Pouncing units use PlayReaction, Quick-Draw gear uses
    // PlayReaction, and activated abilities with [Action] timing use
    // ActivateActionAbility. All three silently no-op'd until the
    // 100-action safety cap fired.
    //
    // The same "play a thing, reset passes, focus to opponent" shape
    // applies to all of these. We dispatch via executeIntent (which
    // already routes each type to the correct executor) and reset
    // focus passes.
    switch (chosen.type) {
        case IntentType::PlayActionCard:     // Action spells + Ambush units
        case IntentType::PlayReaction:       // Pouncing units, Quick-Draw gear, Reaction spells in showdown
        case IntentType::PlayCard:           // generic play (safety)
        case IntentType::ActivateActionAbility: // [Action] [E]: abilities
        case IntentType::ActivateReactionAbility: // [Reaction] [E]: abilities
        case IntentType::ActivateAbility: {  // generic activate (safety)
            state_.turn.players_passed_focus.clear();  // reset passes
            // A chain priority decision inside this action resumes with
            // resumeChain: the chain, this Cleanup, then the focus loop.
            state_.turn.chain_step = ChainStep::ShowdownAction;
            state_.turn.chain_step_bf = bf_id;
            executeIntent(chosen);
            state_.turn.chain_step = ChainStep::None;
            state_.turn.chain_step_bf.reset();
            cleanup();
            // After chain resolves, focus passes to opponent
            return opponent(current_focus);
        }
        default:
            // Any other intent type — fall through with focus unchanged
            // so the outer loop can try again. Mirrors the legacy body's
            // permissive handling of unexpected intents.
            return current_focus;
    }
}

void GameEngine::runShowdownLoop(BattlefieldId bf_id, bool resume) {
    // Focus passing loop (CR 313, CR 348.1).
    // Turn player gets focus first. Players alternate.
    // When both pass focus consecutively without adding chain items, showdown closes.
    //
    // Bridge: legacy path calls queryAgent between advance + resolve.
    // The step-machine path (C-1 commit 9) will surface advance's legal
    // actions out to applyChoice() and feed the chosen intent back into
    // resolve — no thread, no condvar.
    //
    // Resuming a snapshot taken at a focus decision keeps its focus holder
    // and the passes already made.
    PlayerId current_focus;
    if (resume) {
        current_focus = *state_.turn.focus_holder;
        state_.turn.awaiting_focus_at.reset();
    } else {
        state_.turn.players_passed_focus.clear();
        current_focus = state_.turn.turn_player;
    }

    int action_count = 0;
    while (action_count < kMaxShowdownActions && !state_.game_over) {
        auto adv = advanceShowdown(bf_id, current_focus, action_count);
        if (adv.kind == ShowdownAdvance::Kind::Done) return;

        state_.decision_index++;
        // Marks the decision resumeFromSnapshot can re-enter.
        state_.turn.awaiting_focus_at = bf_id;
        auto chosen = getAgent(current_focus).selectAction(state_, adv.legal);
        state_.turn.awaiting_focus_at.reset();
        recordAppliedIntent(chosen);
        if (on_decision) {
            on_decision(state_, adv.legal, chosen);
        }

        auto next = resolveShowdownDecision(bf_id, chosen, current_focus, action_count);
        if (!next.has_value()) return;
        current_focus = *next;
    }
}

void GameEngine::runCombat(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);
    bf.combat_in_progress = true;

    // Determine attacker/defender (CR 459.2.b)
    bf.attacker = bf.contested_by;
    bf.defender = opponent(bf.contested_by);
    events_.logTrace("COMBAT: BF#" + std::to_string(bf_id) + " attacker=" +
                     toString(*bf.attacker) + " defender=" + toString(*bf.defender));

    // Assign combat designations to units
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.isAtBattlefield()) continue;
        auto unit_bf = obj.battlefieldId();
        if (!unit_bf || *unit_bf != bf_id) continue;

        if (obj.controller == *bf.attacker) {
            obj.combat_designation = CombatDesignation::Attacker;
        } else if (obj.controller == *bf.defender) {
            obj.combat_designation = CombatDesignation::Defender;
        }
    }

    // Recompute might (assault/shield apply based on designation)
    for (auto& [id, obj] : state_.objects) {
        if (obj.isUnit() && obj.combat_designation != CombatDesignation::None) {
            obj.recomputeMight();
        }
    }

    for (PlayerId p : {*bf.attacker, *bf.defender}) {
        auto& cps = state_.player(p);
        cps.last_combat_bf = bf_id;
        cps.last_combat_attacker = (p == *bf.attacker);
        cps.excess_combat_damage = 0;
    }
    events_.emit(CombatStartedEvent{bf_id, *bf.attacker, *bf.defender});

    // Drain any WhenIAttack / WhenIDefend / WhenIAttackOrDefend triggers
    // that the CombatStartedEvent dispatched to the chain (CR 459.2.d).
    // Pre-2026-05-19 engine-audit CRITICAL #2: addAbility put these on
    // the chain but nothing called runChain() — they sat until a
    // showdown spell happened to be played, OR until the post-combat
    // mainPhase resumed. Effects intended to fire BEFORE combat damage
    // (kill an attacker, bounce a defender) were silently delayed.
    //
    // Process FEPR now so the triggers (and any Reactions players have
    // for them) resolve fully before we enter the showdown decision
    // loop. Players still get full priority windows because
    // processFEPR handles Closed State priority passing internally.
    if (!state_.chain.items.empty()) {
        runChainInStep(ChainStep::CombatStart, bf_id);
    }
    continueCombat(bf_id);
}

void GameEngine::continueCombat(BattlefieldId bf_id) {
    // The chain may have added a battlefield, so look this one up again.
    auto& bf = getBattlefield(bf_id);

    // Re-check combat viability AFTER triggers resolve — kills or
    // bounces in trigger resolution may have emptied one side.
    {
        auto att_after_trig = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.attacker);
        auto def_after_trig = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.defender);
        if (att_after_trig.empty() || def_after_trig.empty()) {
            bf.combat_phase = CombatPhase::ResolutionStep;
            combatResolutionStep(bf_id);
            return;
        }
    }

    // Step 1: Combat Showdown — players can play Action/Reaction spells
    bf.combat_phase = CombatPhase::ShowdownStep;
    runShowdownLoop(bf_id);
    finishCombatAfterShowdown(bf_id);
}

void GameEngine::finishCombatAfterShowdown(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);
    // After showdown: check if combat can continue (CR 460.1)
    // Damage step only occurs if both sides still have units
    auto att_check = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.attacker);
    auto def_check = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.defender);

    if (att_check.empty() || def_check.empty()) {
        // One side was eliminated during showdown — skip damage, go to resolution
        bf.combat_phase = CombatPhase::ResolutionStep;
        combatResolutionStep(bf_id);
        return;
    }

    // Step 2: Combat Damage
    bf.combat_phase = CombatPhase::DamageStep;
    combatDamageStep(bf_id);

    // Step 3: Resolution
    bf.combat_phase = CombatPhase::ResolutionStep;
    combatResolutionStep(bf_id);
}

void GameEngine::combatDamageStep(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);

    auto att_units = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.attacker);
    auto def_units = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.defender);

    if (att_units.empty() || def_units.empty()) return;

    {
        std::string att_str, def_str;
        for (auto uid : att_units) {
            auto& u = state_.getObject(uid);
            if (!att_str.empty()) att_str += ", ";
            att_str += u.name + "(" + std::to_string(u.current_might) + "M" +
                       (u.is_stunned ? ",STUNNED" : "") + ")";
        }
        for (auto uid : def_units) {
            auto& u = state_.getObject(uid);
            if (!def_str.empty()) def_str += ", ";
            def_str += u.name + "(" + std::to_string(u.current_might) + "M" +
                       (u.is_stunned ? ",STUNNED" : "") + ")";
        }
        events_.logTrace("DAMAGE_STEP: BF#" + std::to_string(bf_id) +
                         " ATK=[" + att_str + "] DEF=[" + def_str + "]");
    }

    // Reorder targets: Tank first, Backline last (CR 460.2.c)
    auto sortByTankBackline = [&](std::vector<GameObjectId>& units) {
        std::stable_sort(units.begin(), units.end(),
            [&](GameObjectId a, GameObjectId b) {
                auto& oa = state_.getObject(a);
                auto& ob = state_.getObject(b);
                bool a_tank = oa.hasKeyword(Keyword::Tank);
                bool b_tank = ob.hasKeyword(Keyword::Tank);
                bool a_back = oa.hasKeyword(Keyword::Backline);
                bool b_back = ob.hasKeyword(Keyword::Backline);
                // Tank before non-Tank, non-Backline before Backline
                if (a_tank != b_tank) return a_tank > b_tank;
                if (a_back != b_back) return a_back < b_back;
                return false;
            });
    };
    sortByTankBackline(att_units);
    sortByTankBackline(def_units);

    // Sum attacker might (CR 460.2.a) — stunned units contribute 0 (CR 423)
    int att_might = 0;
    for (auto uid : att_units) {
        auto& u = state_.getObject(uid);
        if (!u.dealsNoCombatDamage()) att_might += u.current_might;
    }

    // Sum defender might (CR 460.2.b)
    int def_might = 0;
    for (auto uid : def_units) {
        auto& u = state_.getObject(uid);
        if (!u.dealsNoCombatDamage()) def_might += u.current_might;
    }

    // Both players assign damage to opponent's units (CR 460.2.c).
    // Tank must be assigned lethal before non-Tank. Backline assigned last.
    // Agent chooses distribution within those constraints.
    //
    // Bridge: legacy path calls queryAgent between advance + resolve.
    // The step-machine path (C-1 commit 9) will surface advance's legal
    // options out to applyChoice() and feed the chosen Intent back into
    // resolve.
    auto runOneSide = [&](PlayerId assigner, int total_damage,
                          const std::vector<GameObjectId>& targets) {
        auto adv = advanceCombatDamage(assigner, total_damage, targets);
        if (adv.kind == CombatDamageAdvance::Kind::Skip) return;
        state_.decision_index++;
        auto chosen = getAgent(assigner).selectAction(state_, adv.legal);
        recordAppliedIntent(chosen);
        if (on_decision) {
            on_decision(state_, adv.legal, chosen);
        }
        resolveCombatDamageDecision(chosen);
    };

    // Attacker assigns damage to defender's units, defender assigns to attacker's
    runOneSide(*bf.attacker, att_might, def_units);
    runOneSide(*bf.defender, def_might, att_units);
}

GameEngine::CombatDamageAdvance GameEngine::testHook_advanceCombatDamage(
    PlayerId assigner, int total_damage,
    const std::vector<GameObjectId>& targets) {
    return advanceCombatDamage(assigner, total_damage, targets);
}

GameEngine::CombatDamageAdvance GameEngine::advanceCombatDamage(
    PlayerId assigner, int total_damage,
    const std::vector<GameObjectId>& targets) {

    CombatDamageAdvance adv;
    adv.assigner = assigner;
    if (total_damage <= 0 || targets.empty()) {
        adv.kind = CombatDamageAdvance::Kind::Skip;
        return adv;
    }

    // Build default assignment (greedy lethal, respecting caller's
    // Tank/Backline ordering on `targets`).
    const bool any_damage_lethal = anyDamageIsLethalFor(assigner);
    auto buildDefault = [&]() -> std::vector<DamageAssignment> {
        return lethalCascade(state_, targets, total_damage, any_damage_lethal);
    };

    // CR 460.2.c.3 (strict lethality) + 460.2.c.4 (no over-assign) + CR 815
    // (Tank first) + CR 826 (Backline last): the only CR-legal damage
    // allocations are greedy-lethal cascades, varying only by the WITHIN-
    // bucket order of targets (CR 460.2.c.6 — any order within tied
    // priority). We partition into Tank / non-Tank-non-Backline / Backline
    // buckets, enumerate permutations within each, concatenate, and run
    // greedy-lethal on every ordering. Identical resulting allocations
    // are deduped.
    // Dune Surfer: "You ignore [Tank] while assigning combat damage here."
    bool ignore_tank = false;
    if (!targets.empty()) {
        if (auto tbf = state_.getObject(targets.front()).battlefieldId())
            for (const auto& b : state_.battlefields)
                if (b.id == *tbf) ignore_tank = b.ignore_tank[playerIndex(assigner)];
    }
    std::vector<GameObjectId> tanks, mids, backs;
    for (auto tid : targets) {
        auto& obj = state_.getObject(tid);
        if (obj.hasKeyword(Keyword::Tank) && !ignore_tank) tanks.push_back(tid);
        else if (obj.hasKeyword(Keyword::Backline)) backs.push_back(tid);
        else                                        mids.push_back(tid);
    }

    auto greedyCascade = [&](const std::vector<GameObjectId>& order)
        -> std::vector<DamageAssignment> {
        return lethalCascade(state_, order, total_damage, any_damage_lethal);
    };

    auto canonicalKey = [](const std::vector<DamageAssignment>& da) {
        // Order-independent key: sort by (target, damage) so two cascades
        // that produce the same FINAL marked-damage pattern dedupe to one.
        std::vector<std::pair<GameObjectId,int>> kv;
        for (auto& a : da) if (a.damage > 0) kv.emplace_back(a.target_unit, a.damage);
        std::sort(kv.begin(), kv.end());
        std::string s;
        for (auto& [t, d] : kv) {
            s += std::to_string(t); s += ':'; s += std::to_string(d); s += ',';
        }
        return s;
    };

    std::vector<Intent>& options = adv.legal;
    std::set<std::string> seen;

    auto emitIfNew = [&](std::vector<DamageAssignment>&& da) {
        auto key = canonicalKey(da);
        if (seen.insert(key).second) {
            options.push_back(Intent::assignCombatDamage(assigner, std::move(da)));
        }
    };

    // Enumerate permutations within each bucket, then concatenate.
    // std::next_permutation requires sorted input to traverse all
    // permutations. For small bucket sizes (typical combat: 0–4 units
    // per bucket) the permutation counts stay tiny (n! with n≤4 → ≤24).
    // Enumerate the distinct outcomes directly instead of permuting every
    // unit: walk the buckets in order, and at each step choose which remaining
    // unit of the current bucket takes lethal next, until the damage runs out.
    // Two orders that have killed the same set of units lead to the same
    // continuations, so each set is expanded once. (Permuting whole buckets is
    // factorial: 16 token defenders never finished.)
    std::vector<std::vector<GameObjectId>> buckets = {tanks, mids, backs};
    for (auto& b : buckets) std::sort(b.begin(), b.end());
    std::vector<GameObjectId> flat;
    std::vector<int> bucket_of;
    for (int bi = 0; bi < 3; ++bi)
        for (auto id : buckets[bi]) {
            if (!canTakeCombatDamage(state_.getObject(id))) continue;   // CR 467.2.c.10
            flat.push_back(id);
            bucket_of.push_back(bi);
        }

    // Bounds: the option list feeds an agent's action space, and the search
    // must stay cheap on huge boards. Past either bound the remaining orders
    // are not offered; the default cascade is always present.
    constexpr size_t kMaxDamageOptions = 64;
    constexpr int kMaxSearchNodes = 20000;
    emitIfNew(buildDefault());

    if (flat.size() <= 60) {
        std::set<uint64_t> expanded;
        int nodes = 0;
        std::vector<DamageAssignment> cur;
        std::function<void(uint64_t, int)> walk = [&](uint64_t taken, int remaining) {
            if (options.size() >= kMaxDamageOptions || ++nodes > kMaxSearchNodes) return;
            // Current bucket = the first one that still has an untaken unit.
            int bucket = -1;
            for (size_t k = 0; k < flat.size(); ++k)
                if (!(taken >> k & 1)) { bucket = bucket_of[k]; break; }
            if (remaining <= 0 || bucket < 0) {
                auto da = cur;
                if (remaining > 0 && !da.empty()) da.back().damage += remaining;
                if (!da.empty()) emitIfNew(std::move(da));
                return;
            }
            for (size_t k = 0; k < flat.size(); ++k) {
                if ((taken >> k & 1) || bucket_of[k] != bucket) continue;
                const auto& t = state_.getObject(flat[k]);
                const int assigned =
                    std::min(remaining, combatDamageNeededForLethal(t, any_damage_lethal));
                const uint64_t next = taken | (uint64_t{1} << k);
                const int left = remaining - assigned;
                // Same set fully served -> same continuations: expand once.
                if (left > 0 && !expanded.insert(next).second) continue;
                cur.push_back({flat[k], assigned});
                walk(next, left);
                cur.pop_back();
                if (options.size() >= kMaxDamageOptions || nodes > kMaxSearchNodes) return;
            }
        };
        walk(0, total_damage);
    }

    adv.kind = CombatDamageAdvance::Kind::NeedDecision;
    return adv;
}

void GameEngine::resolveCombatDamageDecision(const Intent& chosen) {
    int total_damage = 0;
    for (auto& da : chosen.damage_assignments) total_damage += da.damage;

    events_.logTrace(std::string("ASSIGN_DAMAGE: ") + toString(chosen.player) +
                     " assigns " + std::to_string(total_damage) + " damage:");
    for (auto& da : chosen.damage_assignments) {
        if (da.damage > 0) {
            auto& t = state_.getObject(da.target_unit);
            events_.logTrace("  -> " + t.name + " takes " +
                             std::to_string(da.damage) + " (was " +
                             std::to_string(t.damage_marked) + "/" +
                             std::to_string(t.current_might) + "M)");
            applyCombatDamage(da.target_unit, da.damage);
        }
    }
}

void GameEngine::combatResolutionStep(BattlefieldId bf_id) {
    auto& bf = getBattlefield(bf_id);

    // Combat Cleanup: heal all units, kill lethally damaged (CR 461.1.a)
    processLethalDamage();

    // Heal surviving units (CR 461.1.a.1)
    for (auto& [id, obj] : state_.objects) {
        if (obj.isUnit() && obj.isAtBattlefield()) {
            auto unit_bf = obj.battlefieldId();
            if (unit_bf && *unit_bf == bf_id) {
                obj.damage_marked = 0;
            }
        }
    }

    // Recall attackers if defenders still present (CR 461.1.a.2)
    auto att_remaining = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.attacker);
    auto def_remaining = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.defender);

    const bool recalled = !att_remaining.empty() && !def_remaining.empty();
    if (recalled) {
        // Both still have units = a tie. Normally only attackers recall
        // (CR 461.1.a.2). Symbol of the Solari (227): if the attacker controls
        // it, recall ALL units (attackers AND defenders) instead.
        const bool recall_all = state_.player(*bf.attacker).recall_all_on_attacker_tie;
        for (auto uid : att_remaining) {
            moveUnit(uid, BaseLocation{*bf.attacker});
            events_.emit(UnitMovedEvent{uid, *bf.attacker,
                BattlefieldLocation{bf_id}, BaseLocation{*bf.attacker}, false});
        }
        if (recall_all) {
            for (auto uid : def_remaining) {
                moveUnit(uid, BaseLocation{*bf.defender});
                events_.emit(UnitMovedEvent{uid, *bf.defender,
                    BattlefieldLocation{bf_id}, BaseLocation{*bf.defender}, false});
            }
            events_.logTrace("SYMBOL OF THE SOLARI: attacker tie -> recall ALL units");
        }
    }

    // Determine combat result (CR 461.3)
    att_remaining = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.attacker);
    def_remaining = state_.unitsAt(BattlefieldLocation{bf_id}, *bf.defender);

    // The only player with units left here (CR 468.5) — and, unless units
    // were recalled, the combat's winner (CR 468.3.a / 468.3.d).
    PlayerId holder = PlayerId::None;
    if (!att_remaining.empty() && def_remaining.empty()) {
        holder = *bf.attacker;
    } else if (att_remaining.empty() && !def_remaining.empty()) {
        holder = *bf.defender;
    }
    const PlayerId combat_winner = recalled ? PlayerId::None : holder;

    events_.logTrace("COMBAT_RESULT: BF#" + std::to_string(bf_id) + " " +
                     (combat_winner == PlayerId::None ? std::string("no result")
                                                      : std::string(toString(combat_winner)) + " wins") +
                     " (attackers left " + std::to_string(att_remaining.size()) +
                     ", defenders left " + std::to_string(def_remaining.size()) + ")");

    // Clear combat designations
    for (auto& [id, obj] : state_.objects) {
        if (obj.combat_designation != CombatDesignation::None) {
            obj.combat_designation = CombatDesignation::None;
            obj.recomputeMight();
        }
    }

    // End combat
    bf.combat_in_progress = false;
    bf.combat_phase = CombatPhase::None;
    bf.is_contested = false;
    bf.attacker = std::nullopt;
    bf.defender = std::nullopt;

    // "No Result" (CR 468.3.d) is an empty winner, not PlayerId::None:
    // a None winner would reach every "when you win a combat" legend.
    std::optional<PlayerId> result;
    if (combat_winner != PlayerId::None) result = combat_winner;
    events_.emit(CombatEndedEvent{bf_id, result});

    // Establish control (CR 468.5): whoever has units remaining, won or not.
    if (holder != PlayerId::None) {
        auto old_controller = bf.controller;
        bf.controller = holder;
        events_.emit(ControlChangedEvent{bf_id, old_controller, holder});
        bf.conquered_from_uncontrolled = !old_controller.has_value();
        // CR 468.5 / 471.1: a defender who wins already controlled it — no Conquer.
        if (!old_controller.has_value() || *old_controller != holder)
            scoreConquer(holder, bf_id);
    } else if (att_remaining.empty() && def_remaining.empty()) {
        bf.controller = std::nullopt;
    }

    state_.turn.ns_state = NeutralShowdownState::Neutral;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Scoring
// ═══════════════════════════════════════════════════════════════════════════════

bool GameEngine::isScoreGatedByTurn(PlayerId player, BattlefieldId bf) const {
    const auto& b = getBattlefield(bf);
    if (b.min_turn_to_score <= 0) return false;
    return state_.player(player).turns_taken < b.min_turn_to_score;
}

void GameEngine::scoreConquer(PlayerId player, BattlefieldId bf) {
    auto& ps = state_.player(player);

    // Stamp the conquer for "battlefield you conquered this turn" consumers
    // (Perched Grimwyrm). Reaching here means `player` conquered `bf`, even if
    // the point-gain below is blocked/gated.
    {
        auto& bfs = getBattlefield(bf);
        bfs.conquered_on_turn = state_.turn.turn_number;
        bfs.conquered_by_player = player;
    }

    // Continuous "can't gain points" lock (Tianna Crownguard, set via aura).
    if (ps.cannot_gain_points) {
        events_.logTrace(std::string("SCORE_BLOCKED: ") + toString(player) +
                         " can't gain points (Tianna)");
        return;
    }

    // Turn-gated scoring (Forgotten Monument): can't score here until the
    // player has taken `min_turn_to_score` of their own turns.
    if (isScoreGatedByTurn(player, bf)) {
        events_.logTrace(std::string("SCORE_GATED: ") + toString(player) +
                         " can't score BF#" + std::to_string(bf) + " until turn " +
                         std::to_string(getBattlefield(bf).min_turn_to_score) +
                         " (turns_taken=" + std::to_string(ps.turns_taken) + ")");
        return;
    }

    // Can only score once per BF per turn (CR 465)
    if (ps.battlefields_scored_this_turn.count(bf)) return;

    ps.battlefields_scored_this_turn.insert(bf);
    events_.logTrace(std::string("CONQUER: ") + toString(player) + " conquers BF#" +
                     std::to_string(bf) + " (score was " + std::to_string(ps.score) + ")");

    // Otterpus: "If a player would score 1 point from conquering or holding
    // during their first or second turn, they draw 1 instead." The conquer /
    // hold still happened (its triggers fire via ScoreEvent); only the point
    // is replaced.
    if (state_.turn.early_score_draws_instead && ps.turns_taken <= 2) {
        events_.logTrace(std::string("SCORE_REPLACED: ") + toString(player) +
                         " draws 1 instead of scoring (turn " +
                         std::to_string(ps.turns_taken) + ")");
        drawCards(player, 1);
        events_.emit(ScoreEvent{player, bf, ScoreMethod::Conquer, ps.score});
        return;
    }

    if (isWinningPointAttempt(player)) {
        if (canGainWinningPointViaConquer(player)) {
            ps.score++;
            events_.logTrace(std::string("SCORE: ") + toString(player) + " +1 via Conquer -> " +
                             std::to_string(ps.score));
            checkWinCondition();   // the game ends as the point is scored, before its triggers
            events_.emit(ScoreEvent{player, bf, ScoreMethod::Conquer, ps.score});
        } else {
            events_.logTrace(std::string("SCORE_BLOCKED: ") + toString(player) +
                             " winning point blocked (not all BFs scored), draw 1 instead");
            // Draw a card instead (CR 466.1.b.2)
            drawCards(player, 1);
        }
    } else {
        ps.score++;
        events_.logTrace(std::string("SCORE: ") + toString(player) + " +1 via Conquer -> " +
                         std::to_string(ps.score));
        checkWinCondition();   // the game ends as the point is scored, before its triggers
        events_.emit(ScoreEvent{player, bf, ScoreMethod::Conquer, ps.score});
    }

    checkWinCondition();
}

void GameEngine::scoreHold(PlayerId player, BattlefieldId bf) {
    auto& ps = state_.player(player);

    // [Deploy] (CR 830.1.b): "When an opponent holds here, kill this."
    {
        std::vector<GameObjectId> deployed;
        for (const auto& [id, obj] : state_.objects)
            if (obj.controller != player && obj.card_def_id != kInvalidId &&
                !obj.attached_to.has_value() && obj.battlefieldId() == bf &&
                card_db_.get(obj.card_def_id).deploy)
                deployed.push_back(id);
        for (auto id : deployed) {
            if (!state_.objectExists(id) || !state_.getObject(id).location.has_value()) continue;
            events_.logTrace("DEPLOY: " + state_.getObject(id).name +
                             " killed (an opponent holds its battlefield)");
            effect_executor_->killObject(id);
        }
    }

    // Continuous "can't gain points" lock (Tianna Crownguard, set via aura).
    if (ps.cannot_gain_points) {
        events_.logTrace(std::string("SCORE_BLOCKED: ") + toString(player) +
                         " can't gain points (Tianna)");
        return;
    }

    if (isScoreGatedByTurn(player, bf)) {
        events_.logTrace(std::string("SCORE_GATED: ") + toString(player) +
                         " can't score BF#" + std::to_string(bf) + " until turn " +
                         std::to_string(getBattlefield(bf).min_turn_to_score) +
                         " (turns_taken=" + std::to_string(ps.turns_taken) + ")");
        return;
    }

    if (ps.battlefields_scored_this_turn.count(bf)) return;
    ps.battlefields_scored_this_turn.insert(bf);

    // Otterpus: "If a player would score 1 point from conquering or holding
    // during their first or second turn, they draw 1 instead." The conquer /
    // hold still happened (its triggers fire via ScoreEvent); only the point
    // is replaced.
    if (state_.turn.early_score_draws_instead && ps.turns_taken <= 2) {
        events_.logTrace(std::string("SCORE_REPLACED: ") + toString(player) +
                         " draws 1 instead of scoring (turn " +
                         std::to_string(ps.turns_taken) + ")");
        drawCards(player, 1);
        events_.emit(ScoreEvent{player, bf, ScoreMethod::Hold, ps.score});
        return;
    }

    // Hold always grants Winning Point (CR 466.1.b.1)
    ps.score++;
    ps.hold_points_this_turn++;
    events_.logTrace(std::string("SCORE: ") + toString(player) + " +1 via Hold BF#" +
                     std::to_string(bf) + " -> " + std::to_string(ps.score));
    checkWinCondition();   // the game ends as the point is scored, before its triggers
    events_.emit(ScoreEvent{player, bf, ScoreMethod::Hold, ps.score});

    checkWinCondition();
}

bool GameEngine::isWinningPointAttempt(PlayerId player) const {
    return state_.player(player).score >= state_.mode.victory_score - 1;
}

bool GameEngine::canGainWinningPointViaConquer(PlayerId player) const {
    // Must have scored every battlefield this turn (CR 466.1.b.2)
    return state_.player(player).battlefields_scored_this_turn.size() >=
           state_.totalBattlefields();
}

} // namespace riftbound
