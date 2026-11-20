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
// Turn loop
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::runTurnLoop() {
    // Safety limit to prevent infinite games
    constexpr int kMaxTurns = 200;

    while (!state_.game_over && state_.turn.turn_number < kMaxTurns) {
        runTurn(state_.turn.turn_player);

        if (state_.game_over) break;

        // Check for additional turns (CR 734-738)
        // Check both players' queues
        auto& p1 = state_.players[0];
        auto& p2 = state_.players[1];
        if (!p1.additional_turns.empty()) {
            auto next = p1.additional_turns.front();
            p1.additional_turns.erase(p1.additional_turns.begin());
            state_.turn.turn_player = next;
            state_.turn.is_additional_turn = true;
            events_.logTrace(std::string("ADDITIONAL_TURN: ") + toString(next));
        } else if (!p2.additional_turns.empty()) {
            auto next = p2.additional_turns.front();
            p2.additional_turns.erase(p2.additional_turns.begin());
            state_.turn.turn_player = next;
            state_.turn.is_additional_turn = true;
            events_.logTrace(std::string("ADDITIONAL_TURN: ") + toString(next));
        } else {
            // Normal turn alternation
            state_.turn.turn_player = opponent(state_.turn.turn_player);
            state_.turn.is_additional_turn = false;
        }
        state_.turn.turn_number++;
    }

    if (!state_.game_over) {
        // Max turns reached — draw
        state_.game_over = true;
        state_.winner = PlayerId::None;
        state_.game_over_reason = "Max turns reached";
        events_.emit(GameOverEvent{PlayerId::None, "Max turns reached"});
    }
}

void GameEngine::runTurn(PlayerId player) {
    events_.logTrace("════ TURN " + std::to_string(state_.turn.turn_number) +
                     " (" + toString(player) + ") ════ Score: P1=" +
                     std::to_string(state_.players[0].score) + " P2=" +
                     std::to_string(state_.players[1].score));
    // "This turn" tracking is per turn, not per player: both players' counters
    // reset when any turn starts.
    state_.player(PlayerId::Player1).resetTurnTracking();
    state_.player(PlayerId::Player2).resetTurnTracking();
    state_.turn.turn_decisions_p1 = 0;
    state_.turn.turn_decisions_p2 = 0;
    state_.turn.any_unit_died_this_turn = false;

    awakenPhase();
    if (state_.game_over) return;

    beginningStep();
    if (state_.game_over) return;

    scoringStep();
    if (state_.game_over) return;

    channelPhase();
    if (state_.game_over) return;

    drawPhase();
    if (state_.game_over) return;

    mainPhase();
    if (state_.game_over) return;

    endingStep();
    if (state_.game_over) return;

    expirationStep();

    // Mark first turn as done
    state_.player(player).is_first_turn = false;
}

// Resume a turn that's already in progress. Used by resumeFromSnapshot
// after the clone substitutes the snapshotted GameState into a fresh
// engine. Skips phases that have already completed (per resume_at) and
// runs the rest in order. Per-turn tracking is NOT reset here (already
// captured in the snapshot).
//
// Fallthrough switch — each phase falls through to the next so we
// finish the turn from wherever it was paused.
void GameEngine::runTurnFromPhase(PlayerId player, TurnPhase resume_at) {
    events_.logTrace("════ RESUME TURN " + std::to_string(state_.turn.turn_number) +
                     " (" + toString(player) + ") at phase " +
                     toString(resume_at) +
                     " ════ Score: P1=" +
                     std::to_string(state_.players[0].score) + " P2=" +
                     std::to_string(state_.players[1].score));

    // A snapshot taken at a chain priority decision outside the main phase
    // first finishes that chain and the step it ran in; the turn then goes on
    // from the next step.
    if (resume_at != TurnPhase::MainPhase && state_.chain.awaiting_priority) {
        resumeChain();
        if (state_.game_over) return;
        switch (resume_at) {
            case TurnPhase::BeginningStep: resume_at = TurnPhase::ScoringStep; break;
            case TurnPhase::ScoringStep:   resume_at = TurnPhase::ChannelPhase; break;
            case TurnPhase::EndingStep:    resume_at = TurnPhase::ExpirationStep; break;
            case TurnPhase::ExpirationStep:
                state_.player(player).is_first_turn = false;
                return;
            default:
                throw std::logic_error("runTurnFromPhase: a chain to resume in phase " +
                                       std::string(toString(resume_at)));
        }
    }

    switch (resume_at) {
        case TurnPhase::AwakenPhase:
            awakenPhase();
            if (state_.game_over) return;
            [[fallthrough]];
        case TurnPhase::BeginningStep:
            // Only run beginningStep if we're resuming AT or BEFORE it.
            // (When resume_at is later, we drop through this case.)
            if (resume_at == TurnPhase::BeginningStep ||
                resume_at == TurnPhase::AwakenPhase) {
                beginningStep();
                if (state_.game_over) return;
            }
            [[fallthrough]];
        case TurnPhase::ScoringStep:
            if (resume_at == TurnPhase::ScoringStep ||
                resume_at == TurnPhase::BeginningStep ||
                resume_at == TurnPhase::AwakenPhase) {
                scoringStep();
                if (state_.game_over) return;
            }
            [[fallthrough]];
        case TurnPhase::ChannelPhase:
            if (resume_at == TurnPhase::ChannelPhase ||
                resume_at == TurnPhase::ScoringStep ||
                resume_at == TurnPhase::BeginningStep ||
                resume_at == TurnPhase::AwakenPhase) {
                channelPhase();
                if (state_.game_over) return;
            }
            [[fallthrough]];
        case TurnPhase::DrawPhase:
            if (resume_at == TurnPhase::DrawPhase ||
                resume_at == TurnPhase::ChannelPhase ||
                resume_at == TurnPhase::ScoringStep ||
                resume_at == TurnPhase::BeginningStep ||
                resume_at == TurnPhase::AwakenPhase) {
                drawPhase();
                if (state_.game_over) return;
            }
            [[fallthrough]];
        case TurnPhase::MainPhase:
            // When resuming AT MainPhase, skip the entry side effects
            // (mainPhase() resets ns_state/oc_state/priority_holder
            // and emits PhaseChangedEvent — both clobber the snapshot).
            // Just re-enter the action loop. Once the loop drains, fall
            // through to endingStep as normal. A snapshot taken at a
            // Showdown focus decision first finishes that Showdown; one
            // taken at a chain priority decision, the chain and its step.
            // Entered from an earlier step, the main phase starts as usual.
            if (resume_at != TurnPhase::MainPhase) {
                mainPhase();
            } else {
                if (state_.chain.awaiting_priority) {
                    resumeChain();
                    if (state_.game_over) return;
                } else if (state_.turn.awaiting_focus_at.has_value()) {
                    resumeShowdown(*state_.turn.awaiting_focus_at);
                    if (state_.game_over) return;
                }
                mainPhaseLoop();
            }
            if (state_.game_over) return;
            [[fallthrough]];
        case TurnPhase::EndingStep:
            endingStep();
            if (state_.game_over) return;
            [[fallthrough]];
        case TurnPhase::ExpirationStep:
            expirationStep();
            break;
        default:
            // Setup / Mulligan / GameOver — caller shouldn't call us here.
            throw std::logic_error(
                "GameEngine::runTurnFromPhase called with phase that has no "
                "turn-internal resume point: " + std::string(toString(resume_at)));
    }
    state_.player(player).is_first_turn = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phases
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::awakenPhase() {
    events_.logTrace("── Awaken Phase ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::AwakenPhase;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::AwakenPhase,
                                    state_.turn.turn_player});

    // Count this player's own turns (drives turn-gated rules, e.g. Forgotten
    // Monument). 1 on their first turn.
    state_.player(state_.turn.turn_player).turns_taken++;

    // Ready all game objects the turn player controls (CR 315.1.b)
    auto player = state_.turn.turn_player;
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller == player && obj.is_exhausted && obj.location.has_value()) {
            // "Units don't ready during the Awaken Phase." (Firebrand Raider)
            if (state_.turn.no_awaken_unit_ready && obj.isUnit()) continue;
            if (const Card* c = card_registry_.get(obj.card_def_id); c && c->cantBeReadied()) continue;
            obj.is_exhausted = false;
            events_.emit(ObjectStateChangedEvent{id, "readied"});
            events_.emit(ObjectStateChangedEvent{id, "became_ready"});
        }
    }
    // Also ready the legends
    for (auto legend_id : state_.player(player).legends()) {
        if (!state_.objectExists(legend_id)) continue;
        auto& legend = state_.getObject(legend_id);
        if (legend.is_exhausted) {
            legend.is_exhausted = false;
            events_.emit(ObjectStateChangedEvent{legend_id, "readied"});
        }
    }
}

void GameEngine::beginningStep() {
    events_.logTrace("── Beginning Step ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::BeginningStep;

    // Kill Temporary permanents before scoring (CR: "Kill me at the start of
    // my controller's Beginning Phase, before scoring"). Applies to both units
    // AND gear — Fading Memories (180) can grant [Temporary] to either.
    std::vector<GameObjectId> units_to_kill;
    std::vector<GameObjectId> gear_to_kill;
    for (auto& [id, obj] : state_.objects) {
        if (!obj.keywords.has(Keyword::Temporary)) continue;
        if (obj.controller != state_.turn.turn_player) continue;
        if (!obj.location.has_value()) continue;
        if (obj.isUnit()) units_to_kill.push_back(id);
        else if (obj.isGear()) gear_to_kill.push_back(id);
    }
    state_.death_batch++;
    for (auto id : units_to_kill) {
        killUnit(id);
    }
    for (auto id : gear_to_kill) {
        effect_executor_->killObject(id);  // gear-aware kill (Banishment/trash)
    }

    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::BeginningStep,
                                    state_.turn.turn_player});

    // Process "At the start of your Beginning Phase" triggers
    if (chain_manager_->chainExists()) runChainInStep(ChainStep::BeginningStep);

    // CR 319: a Cleanup follows these state changes, before the Scoring Step.
    // Without it a battlefield whose last unit just died ([Temporary], a
    // start-of-turn kill) is still "controlled" and would wrongly score a Hold.
    cleanupAndRunChain(ChainStep::BeginningStep);
}

// A Cleanup can add triggered abilities (a unit becoming [Mighty], a death). They resolve in
// the step they were added in, with a Cleanup after each, so no step hands a Chain to the next
// (the Main Phase would open with one: CR 309.1).
void GameEngine::cleanupAndRunChain(ChainStep step, std::optional<BattlefieldId> bf) {
    cleanup();
    while (!state_.game_over && chain_manager_->chainExists()) {
        runChainInStep(step, bf);
        if (state_.game_over) return;
        cleanup();
    }
}

void GameEngine::scoringStep() {
    events_.logTrace("── Scoring Step ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::ScoringStep;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::ScoringStep,
                                    state_.turn.turn_player});

    scoreHolds(0);
}

void GameEngine::scoreHolds(size_t first) {
    // Turn player Holds all battlefields they control (CR 315.2.b)
    auto player = state_.turn.turn_player;
    for (size_t i = first; i < state_.battlefields.size(); ++i) {
        const BattlefieldId bf_id = state_.battlefields[i].id;
        const auto& controller = state_.battlefields[i].controller;
        if (controller.has_value() && *controller == player) {
            scoreHold(player, bf_id);
            if (state_.game_over) return;
            // Process hold/score triggers
            if (chain_manager_->chainExists()) runChainInStep(ChainStep::ScoringStep, bf_id);
            if (state_.game_over) return;
        }
    }

    // Resumed after the last battlefield, scoreHolds only finishes this Cleanup and chain.
    if (state_.battlefields.empty()) cleanup();
    else cleanupAndRunChain(ChainStep::ScoringStep, state_.battlefields.back().id);
}

size_t GameEngine::battlefieldIndex(BattlefieldId bf_id) const {
    size_t i = 0;
    while (i < state_.battlefields.size() && state_.battlefields[i].id != bf_id) ++i;
    if (i == state_.battlefields.size())
        throw std::logic_error("no battlefield " + std::to_string(bf_id));
    return i;
}

void GameEngine::channelPhase() {
    events_.logTrace("── Channel Phase ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::ChannelPhase;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::ChannelPhase,
                                    state_.turn.turn_player});

    auto player = state_.turn.turn_player;
    int count = 2;

    // Second player channels an extra rune on first turn (CR 480.7)
    if (state_.player(player).is_first_turn &&
        player != state_.turn.turn_player) {
        // This is the player going second — but wait, on their first turn
        // they ARE the turn player. The rule is about being the second player.
    }
    // Simplified: if this is the second player's first turn, channel 3
    auto first_player = state_.turn.turn_number == 0 ? state_.turn.turn_player
                        : opponent(state_.turn.turn_player);
    // The player who did NOT go first gets the extra channel on their first turn
    if (state_.player(player).is_first_turn && player != first_player) {
        count = 3;
    }
    // Sandstone Chimera: "players only channel 1 rune at the start of their
    // Channel Phase."
    if (state_.turn.channel_phase_cap > 0)
        count = std::min(count, state_.turn.channel_phase_cap);

    channelRunes(player, count);
}

void GameEngine::drawPhase() {
    events_.logTrace("── Draw Phase ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::DrawPhase;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::DrawPhase,
                                    state_.turn.turn_player});

    auto player = state_.turn.turn_player;

    // Skip (CR 444): "Skip your Draw Phase." replaces the phase with nothing —
    // no draw, and nothing that would happen as a result of it.
    if (state_.player(player).skip_draw_phase) {
        events_.logTrace("DRAW PHASE: skipped (CR 444)");
        emptyRunePools();
        return;
    }

    // CR 489.7 / 490.7 / 491.7: in the multiplayer modes the player going
    // first does not draw during their first Draw Phase. In 1v1 (CR 487.7 /
    // 488.7) they draw as normal; the player going second's only
    // adjustment is the extra channel in channelPhase().
    if (state_.mode.first_player_skips_first_draw &&
        state_.player(player).is_first_turn &&
        player == state_.turn.starting_player) {
        events_.logTrace("DRAW PHASE: " + std::string(player == PlayerId::Player1 ? "P1" : "P2") +
                          " skips draw (first turn, going first — CR 489.7)");
    } else {
        // Draw 1 card (CR 315.4.b)
        drawCards(player, 1);
    }

    // Empty rune pools (CR 315.4.d)
    emptyRunePools();
}

// C-1 commit 5 — refactor mainPhase into a step-machine-shaped split.
// Behavior is identical to the prior monolithic loop; the two halves
// (advance / resolve) are the surface that the future step machine
// (C-1 commit 9) will call directly without going through queryAgent.

namespace {
constexpr int kMainPhaseMaxActions = 500;  // safety cap
}  // namespace

GameEngine::MainPhaseAdvance GameEngine::advanceMainPhase(int& action_count) {
    MainPhaseAdvance adv;
    if (state_.game_over || action_count >= kMainPhaseMaxActions) {
        adv.kind = MainPhaseAdvance::Kind::Done;
        return adv;
    }

    // Run staged showdowns/combats before the turn player gets priority. Each
    // one ends with a Cleanup (CR 319.1: back to Neutral Open; 319.6: Units it
    // killed or recalled left the board). That Cleanup removes Contested from
    // a battlefield whose contester has no Units left (CR 323.10) and may
    // stage another showdown or combat, so repeat until nothing is staged.
    constexpr int kMaxStagedRounds = 16;  // safety cap
    for (int round = 0; round < kMaxStagedRounds; ++round) {
        processContestedBattlefields();
        if (state_.game_over) {
            adv.kind = MainPhaseAdvance::Kind::Done;
            return adv;
        }

        const bool ran = runStagedBattlefields(0);
        if (state_.game_over) {
            adv.kind = MainPhaseAdvance::Kind::Done;
            return adv;
        }
        if (!ran) break;

        cleanup();
        if (chain_manager_->chainExists()) runChainInStep(ChainStep::StagedRound);
        if (state_.game_over) {
            adv.kind = MainPhaseAdvance::Kind::Done;
            return adv;
        }
    }

    auto actions = generateLegalActions();
    if (actions.empty()) {
        adv.kind = MainPhaseAdvance::Kind::Done;
        return adv;
    }

    adv.kind = MainPhaseAdvance::Kind::NeedDecision;
    adv.legal = std::move(actions);
    return adv;
}

bool GameEngine::runStagedBattlefields(size_t first) {
    // Sub-calls still use the recursive path until C-1 commit 6 converts the
    // chain / combat subsystems. Indexed: a showdown can create a battlefield.
    bool ran = false;
    for (size_t i = first; i < state_.battlefields.size(); ++i) {
        const BattlefieldId bf_id = state_.battlefields[i].id;
        if (state_.battlefields[i].showdown_staged &&
            !state_.battlefields[i].showdown_in_progress &&
            !state_.battlefields[i].combat_staged) {
            runShowdown(bf_id);
            ran = true;
            if (state_.game_over) return ran;
            state_.battlefields[i].showdown_staged = false;
        }
        if (state_.battlefields[i].combat_staged &&
            !state_.battlefields[i].combat_in_progress) {
            runCombat(bf_id);
            ran = true;
            if (state_.game_over) return ran;
            state_.battlefields[i].combat_staged = false;
        }
    }
    return ran;
}

void GameEngine::resumeShowdown(BattlefieldId bf_id) {
    const size_t i = battlefieldIndex(bf_id);

    // The rest of runStagedBattlefields' body for battlefield i, entered
    // inside the loop the snapshot was taken in.
    if (state_.battlefields[i].combat_in_progress) {
        runShowdownLoop(bf_id, /*resume=*/true);
        finishCombatAfterShowdown(bf_id);
        if (state_.game_over) return;
        state_.battlefields[i].combat_staged = false;
    } else {
        runShowdownLoop(bf_id, /*resume=*/true);
        finishShowdown(bf_id);
        if (state_.game_over) return;
        state_.battlefields[i].showdown_staged = false;
        if (state_.battlefields[i].combat_staged &&
            !state_.battlefields[i].combat_in_progress) {
            runCombat(bf_id);
            if (state_.game_over) return;
            state_.battlefields[i].combat_staged = false;
        }
    }
    finishStagedRound(i);
}

void GameEngine::finishStagedRound(size_t i) {
    runStagedBattlefields(i + 1);
    if (state_.game_over) return;

    // The end of advanceMainPhase's round; mainPhaseLoop's next
    // advanceMainPhase goes on to the next round.
    cleanup();
    if (chain_manager_->chainExists()) runChainInStep(ChainStep::StagedRound);
}

bool GameEngine::resolveMainPhaseDecision(const Intent& intent,
                                          int& action_count) {
    if (intent.type == IntentType::EndTurn) return false;

    // Every chain the action starts runs at the end of executeIntent (or in
    // finishMainAction), so a resumed chain finishes with finishMainAction.
    state_.turn.chain_step = ChainStep::MainAction;
    executeIntent(intent);
    finishMainAction();

    action_count++;
    return !state_.game_over;
}

void GameEngine::finishMainAction() {
    if (state_.turn.chain_step == ChainStep::MainAction) {
        // Process any triggered abilities that were queued during execution
        // (play triggers, spell resolution triggers, etc.)
        if (chain_manager_->chainExists()) runChain();

        cleanup();
        state_.turn.chain_step = ChainStep::MainActionCleanedUp;
    }

    // Process death triggers / cleanup triggers
    if (chain_manager_->chainExists()) runChain();
    state_.turn.chain_step = ChainStep::None;
}

void GameEngine::runChainInStep(ChainStep step, std::optional<BattlefieldId> bf) {
    const ChainStep outer_step = state_.turn.chain_step;
    const std::optional<BattlefieldId> outer_bf = state_.turn.chain_step_bf;
    state_.turn.chain_step = step;
    state_.turn.chain_step_bf = bf;
    runChain();
    state_.turn.chain_step = outer_step;
    state_.turn.chain_step_bf = outer_bf;
}

void GameEngine::resumeChain() {
    const ChainStep step = state_.turn.chain_step;
    runChain(/*resume=*/true);
    if (state_.game_over) return;
    if (step == ChainStep::MainAction || step == ChainStep::MainActionCleanedUp) {
        finishMainAction();
        return;
    }
    // The rest of the step the chain ran in, as the caller of runChainInStep
    // (or resolveShowdownDecision) goes on after it.
    const std::optional<BattlefieldId> bf_id = state_.turn.chain_step_bf;
    state_.turn.chain_step = ChainStep::None;
    state_.turn.chain_step_bf.reset();
    switch (step) {
        case ChainStep::ShowdownAction:
            // The rest of resolveShowdownDecision, then the Showdown loop with
            // focus passed to the opponent of the player who acted.
            cleanup();
            state_.turn.focus_holder = opponent(*state_.turn.focus_holder);
            resumeShowdown(*bf_id);
            break;
        case ChainStep::BeginningStep:
            cleanupAndRunChain(ChainStep::BeginningStep);
            break;
        case ChainStep::EndingStep:
            cleanup();
            break;
        case ChainStep::ScoringStep:
            scoreHolds(battlefieldIndex(*bf_id) + 1);
            break;
        case ChainStep::StagedRound:
            break;  // mainPhaseLoop's next advanceMainPhase starts the next round
        case ChainStep::CombatStart: {
            // The rest of runCombat, then of the staged round it ran in.
            continueCombat(*bf_id);
            if (state_.game_over) return;
            const size_t i = battlefieldIndex(*bf_id);
            state_.battlefields[i].combat_staged = false;
            finishStagedRound(i);
            break;
        }
        case ChainStep::ExpirationStep:
            if (chain_manager_->chainExists()) runExpirationPasses(1);
            break;
        case ChainStep::MainAction:
        case ChainStep::MainActionCleanedUp:
        case ChainStep::None:
            break;
    }
}

void GameEngine::mainPhase() {
    events_.logTrace("── Main Phase ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::MainPhase;
    state_.turn.ns_state = NeutralShowdownState::Neutral;
    state_.turn.oc_state = OpenClosedState::Open;
    state_.turn.priority_holder = state_.turn.turn_player;

    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::MainPhase,
                                    state_.turn.turn_player});

    mainPhaseLoop();
}

// Loop body of mainPhase() — extracted so resumeFromSnapshot can re-enter
// the action loop without firing the entry side effects (which reset
// ns_state/oc_state/priority_holder and re-emit PhaseChangedEvent).
// Those resets would corrupt a snapshot taken mid-chain or mid-showdown.
void GameEngine::mainPhaseLoop() {
    int action_count = 0;
    while (true) {
        auto adv = advanceMainPhase(action_count);
        if (adv.kind == MainPhaseAdvance::Kind::Done) return;

        // Marks the one decision resumeFromSnapshot can re-enter: a resumed
        // engine lands back here, not inside the chosen action.
        state_.turn.awaiting_main_action = true;
        auto intent = queryAgent(state_.turn.turn_player);
        state_.turn.awaiting_main_action = false;
        if (!resolveMainPhaseDecision(intent, action_count)) return;
    }
}

void GameEngine::endingStep() {
    events_.logTrace("── Ending Step ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::EndingStep;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::EndingStep,
                                    state_.turn.turn_player});

    // CR 423.1.a.2 — Stunned units lose Stunned at the beginning of the
    // NEXT Ending Step (i.e., this one). Phase 6q+ correction: previously
    // cleared in expirationStep which is the wrong CR step.
    for (auto& [id, obj] : state_.objects) {
        if (obj.is_stunned) {
            obj.is_stunned = false;
        }
    }

    // Process "At the end of your turn" triggers (e.g., Dazzling Aurora)
    if (chain_manager_->chainExists()) runChainInStep(ChainStep::EndingStep);

    // Cleanup AFTER the chain finishes — same pattern as the main-phase
    // action loop. The end-of-turn chain can damage units (Aurora plays
    // Elder Dragon which damages enemies, Aurora plays a unit whose
    // WhenYouPlayMe damages, etc.). Without this pass, damage sits on
    // damage_marked forever — processLethalDamage doesn't run, so
    // Elder Dragon's "any-damage-lethal" aura never kills the damaged
    // unit, and natural-lethal units (might-1 Mousers etc.) survive
    // into the next turn at 1/1 dmg.
    cleanup();
}

void GameEngine::expireTemporaryKeywords(GameObject& obj, const CardDB& db) {
    if (obj.temp_keywords.bits == 0) return;
    KeywordSet base_keywords;
    if (obj.card_def_id != kInvalidId) {
        base_keywords = db.get(obj.card_def_id).keywords;
    }
    for (int k = 0; k < static_cast<int>(Keyword::Count); ++k) {
        Keyword kw = static_cast<Keyword>(k);
        if (!obj.temp_keywords.has(kw)) continue;
        bool from_base = base_keywords.has(kw);
        bool from_aura = obj.aura_keywords.has(kw);
        if (!from_base && !from_aura) {
            obj.keywords.clear(kw);
        }
    }
    obj.temp_keywords.reset();
}

void GameEngine::expirationStep() {
    events_.logTrace("── Expiration Step ──");
    auto old_phase = state_.turn.phase;
    state_.turn.phase = TurnPhase::ExpirationStep;
    events_.emit(PhaseChangedEvent{old_phase, TurnPhase::ExpirationStep,
                                    state_.turn.turn_player});

    // CR 317.2.f.1 — "If any items underwent the FEPR process, return to
    // the start of the Expiration Step." Loop the body + chain drain
    // until no chain item resolves during a pass. Phase 6q+ fix —
    // pre-fix, expirationStep was a single pass and any triggers fired
    // by healAllUnits / clearStun / emptyRunePools would sit on the chain
    // unresolved or resolve without re-running expiration.
    runExpirationPasses(0);
}

void GameEngine::runExpirationPasses(int first) {
    constexpr int kMaxExpirationPasses = 8;
    for (int pass = first; pass < kMaxExpirationPasses; ++pass) {
        doExpirationBody();
        if (!chain_manager_->chainExists()) break;
        runChainInStep(ChainStep::ExpirationStep);
        // If running the chain produced new chain items (cascading
        // triggers from expiration effects), keep looping. Otherwise
        // we're stable.
        if (!chain_manager_->chainExists()) break;
    }
}

void GameEngine::doExpirationBody() {
    // Heal all units (CR 317.2.b)
    healAllUnits();

    // (Stun clearing moved to endingStep per CR 423.1.a.2)

    // Expire "this turn" effects (CR 317.2.c)
    for (auto& [id, obj] : state_.objects) {
        // Lotus Trap-style damage doubling expires here.
        obj.damage_doubled_this_turn = false;

        // Tactical Retreat's "next time it would die THIS TURN" replacement
        // expires unused at end of turn.
        obj.death_replacement_recall_pending = false;
        obj.death_replacement_recall_by = kInvalidId;

        // Counter Strike's one-shot damage prevention is "this turn".
        obj.prevent_next_damage_this_turn = false;
        obj.prevent_damage_remaining = 0;   // Ki Barrier
        obj.damage_taken_this_turn = 0;
        obj.untargetable_by_enemy_this_turn = false;   // Twilight Shroud
        obj.temp_granted_abilities.clear();            // Dominus

        // "Empower it. Disempower it at end of turn" and the reverse (Sanction,
        // Tornado Warrior).
        if (obj.empower_flip_eot != 0) {
            const int flip = obj.empower_flip_eot;
            obj.empower_flip_eot = 0;
            if (flip > 0) effect_executor_->empower(id);
            else effect_executor_->disempower(id);
        }

        // Per-unit move counter is "this turn" (Kayn, Unleashed).
        obj.moves_this_turn = 0;

        // Temp might is a separate field from buff_count (permanent buffs);
        // just clear it. recomputeMight (below / in cleanup) drops the bonus.
        bool had_temp_might = (obj.temp_might_bonus != 0);
        obj.temp_might_bonus = 0;
        if (obj.temp_assault_value != 0) {
            obj.assault_value -= obj.temp_assault_value;
            obj.temp_assault_value = 0;
            if (obj.assault_value <= 0) {
                obj.assault_value = 0;
                // Only clear keyword if base card doesn't have it
                if (obj.card_def_id != kInvalidId) {
                    const auto& def = card_db_.get(obj.card_def_id);
                    if (!def.keywords.has(Keyword::Assault)) {
                        obj.keywords.clear(Keyword::Assault);
                    }
                } else {
                    obj.keywords.clear(Keyword::Assault);
                }
            }
        }
        if (obj.temp_shield_value != 0) {
            obj.shield_value -= obj.temp_shield_value;
            obj.temp_shield_value = 0;
            if (obj.shield_value <= 0) {
                obj.shield_value = 0;
                if (obj.card_def_id != kInvalidId) {
                    const auto& def = card_db_.get(obj.card_def_id);
                    if (!def.keywords.has(Keyword::Shield)) {
                        obj.keywords.clear(Keyword::Shield);
                    }
                } else {
                    obj.keywords.clear(Keyword::Shield);
                }
            }
        }
        (void)had_temp_might;  // temp fields cleared above; recompute regardless
        obj.recomputeMight();

        // Pure-flag keywords granted "this turn" (Bounty Hunter's
        // Ganking, etc.). Delegated to a static helper so tests can
        // exercise the revoke logic without setting up the engine's
        // full subsystem stack.
        expireTemporaryKeywords(obj, card_db_);
    }
    // Recompute might for all units after expiration
    for (auto& [id, obj] : state_.objects) {
        if (obj.isUnit() && obj.location.has_value()) {
            obj.recomputeMight();
        }
    }

    // Empty rune pools (CR 317.2.d)
    emptyRunePools();

    // CR 416 + "take control of … until end of turn" — revert any
    // EOT-scoped control changes. Phase 6q+ wiring of the
    // EffectExecutor::takeControl primitive (until_end_of_turn=true).
    for (auto& [id, obj] : state_.objects) {
        if (obj.control_reverts_eot) {
            auto restored = obj.original_controller_eot;
            if (restored != PlayerId::None && restored != obj.controller) {
                events_.logTrace(std::string("REVERT_CONTROL: ") + obj.name +
                                 " (id=" + std::to_string(id) + ") " +
                                 toString(obj.controller) + " -> " +
                                 toString(restored));
                obj.controller = restored;
                events_.emit(ObjectStateChangedEvent{id, "controller_reverted"});
            }
            obj.control_reverts_eot = false;
            obj.original_controller_eot = PlayerId::None;
        }
    }

    // Expire delayed abilities that expire at end of turn (CR 389)
    state_.delayed_abilities.erase(
        std::remove_if(state_.delayed_abilities.begin(), state_.delayed_abilities.end(),
            [&](const DelayedAbility& da) {
                return da.expires_on_turn == state_.turn.turn_number;
            }),
        state_.delayed_abilities.end());
}

} // namespace riftbound
