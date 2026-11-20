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

GameEngine::GameEngine(const CardDB& card_db, EventBus& event_bus,
                       const CardRegistry& card_registry)
    : card_db_(card_db), events_(event_bus), card_registry_(card_registry) {
    // Chain subsystems are initialized per-game in runGame

    // Observation tracking. Each CardRevealedEvent updates the
    // observer's PlayerState::observed_cards bank by card_def_id. This
    // lets cards / tests / future ML features verify which identities
    // a player has seen. See CardRevealedEvent in core/events.h for
    // the emit contract; the bump logic lives on GameState::recordReveal
    // so unit tests (which don't construct a GameEngine) can subscribe
    // their own bus to the same helper.
    card_revealed_conn_ = events_.on_card_revealed.connect(
        [this](const CardRevealedEvent& e) { state_.recordReveal(e); });

    // RIFTBOUND_TRACE_STDERR=1 streams every engine log line to stderr as it
    // happens. Runners buffer the log and print it when a game finishes, so
    // this is how to see where a game that never finishes is stuck.
    if (std::getenv("RIFTBOUND_TRACE_STDERR")) {
        trace_stderr_conn_ = events_.on_log.connect([](const LogEvent& e) {
            std::fprintf(stderr, "%s\n", e.message.c_str());
        });
    }

    // Subsystems exist from construction so every engine operation is usable
    // on a hand-built state (tests drive phases and effects without running a
    // whole game). runGame / resumeFromSnapshot rebuild them for the new state.
    initSubsystems();
}

void GameEngine::initSubsystems() {
    // The trigger manager holds a reference to the chain manager and is
    // subscribed to the event bus: drop it first so nothing dangles.
    trigger_manager_.reset();
    chain_manager_ = std::make_unique<ChainManager>(state_, events_, card_db_);
    chain_manager_->setAffordCheck(
        [this](PlayerId p, GameObjectId card) { return canAfford(p, card); });
    chain_manager_->setPayCost(
        [this](PlayerId p, GameObjectId card) { return payCardCost(p, card); });
    effect_executor_ = std::make_unique<EffectExecutor>(state_, events_, card_db_, &card_registry_);
    effect_executor_->setRng(&rng_);
    effect_executor_->setAgentQuery(
        [this](PlayerId p, const std::vector<Intent>& actions) -> Intent {
            return queryAgentForChain(p, actions);
        });
    effect_executor_->setAuraRecalc([this]() { recalculateAuras(); });
    effect_executor_->setQueueSpell([this](PlayerId p, GameObjectId spell) {
        state_.getObject(spell).zone = ZoneType::Chain;
        chain_manager_->addSpell(spell, p, {});
    });
    effect_executor_->setPlayCostFns(
        [this](PlayerId p, GameObjectId card) { return canAfford(p, card); },
        [this](PlayerId p, GameObjectId card) { return payCardCost(p, card); });
    chain_manager_->setEffectExecutor(effect_executor_.get());
    chain_manager_->setFinalizeChoices([this](ChainItem& item) { chooseTargetsAtFinalize(item); });
    chain_manager_->setPlayFromFacedown(
        [this](PlayerId p, GameObjectId c, const std::vector<GameObjectId>& t) {
            playFromFacedown(p, c, t);
        });
    chain_manager_->setActivateAbility(
        [this](const Intent& i) { return payAndQueueAbility(i); });
    chain_manager_->setCleanup([this]() { cleanup(); });
    trigger_manager_ = std::make_unique<TriggerManager>(
        state_, events_, card_db_, *chain_manager_, card_registry_);
    trigger_manager_->setEffectExecutor(effect_executor_.get());
    effect_executor_->setRechoose([this](ChainItemId id) { makeNewChoicesFor(id); });
    effect_executor_->setQueueTrigger(
        [this](GameObjectId source, PlayerId controller, TriggerType t, GameObjectId subject) {
            trigger_manager_->queueTrigger(source, controller, t, subject);
        });
    trigger_manager_->subscribe();
}

GameEngine::~GameEngine() {
    // Disconnect the on_card_revealed subscriber before this is freed.
    // Without this, a successor engine on the same EventBus would
    // emit reveals into a dangling `this`.
    if (card_revealed_conn_.connected()) card_revealed_conn_.disconnect();
    // Fiber refactor: StepDriver's destructor drains the fiber by
    // feeding choice=0 until done. No separate thread to join — the
    // fiber lives in the same call stack as this destructor.
    // step_driver_'s unique_ptr destruction handles it.
}

// ═══════════════════════════════════════════════════════════════════════════════
// Game lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

GameResult GameEngine::runGame(
    const DeckSubmission& deck1,
    const DeckSubmission& deck2,
    AgentInterface& agent1,
    AgentInterface& agent2,
    uint64_t seed) {

    rng_.seed(seed == 0 ? std::random_device{}() : seed);
    agents_[0] = &agent1;
    agents_[1] = &agent2;

    state_ = GameState{};
    state_.mode = ModeOfPlay{};

    initSubsystems();

    setupGame(deck1, deck2);
    drawOpeningHands();
    runMulligans();

    events_.emit(PhaseChangedEvent{TurnPhase::Setup, TurnPhase::AwakenPhase,
                                    state_.turn.turn_player});

    runTurnLoop();

    GameResult result;
    result.winner = state_.winner;
    result.final_scores[0] = state_.players[0].score;
    result.final_scores[1] = state_.players[1].score;
    result.total_turns = state_.turn.turn_number;
    result.total_decisions = state_.decision_index;
    result.termination_reason = state_.game_over_reason;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Step machine API (Phase 11 C-1)
//
// Implementation strategy: this version of the step machine still runs the
// engine on an internal worker thread via StepDriver (Phase C-1 commits
// 1–2 + this wrapper relocation). The threading is now an engine-internal
// implementation detail, not the caller's concern — which lets the
// OpenSpiel wrapper drop its own thread and halting agent.
//
// Subsequent C-1 commits (3–6) replace the threading with a native
// resumable step machine, at which point Clone() can collapse to
// memcpy(GameState). The beginGame/applyChoice/currentStep surface stays
// stable across that transition — callers don't change.
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::refreshStepFromDriver() {
    if (!step_driver_) {
        current_step_.kind = StepKind::Done;
        current_step_.legal.clear();
        current_step_.perspective = PlayerId::None;
        return;
    }
    if (step_driver_->isDone()) {
        current_step_.kind = StepKind::Done;
        current_step_.legal.clear();
        current_step_.perspective = PlayerId::None;
        return;
    }
    current_step_.kind = StepKind::NeedDecision;
    current_step_.legal = step_driver_->legalActionsSnapshot();
    current_step_.perspective = step_driver_->currentPlayerSnapshot();
}

StepResult GameEngine::beginGame(
    const DeckSubmission& deck1,
    const DeckSubmission& deck2,
    uint64_t seed) {
    // Disallow restart on the same engine — too easy to get wrong with
    // a previous session's thread still alive. Make a fresh GameEngine
    // instead.
    if (step_driver_) {
        throw std::logic_error(
            "GameEngine::beginGame called on an engine that already has a "
            "step-machine session. Construct a fresh GameEngine.");
    }
    step_driver_ = std::make_unique<StepDriver>();
    step_result_ = GameResult{};

    // Snapshot decks so the fiber's lambda can capture them by value
    // safely (the fiber may outlive this stack frame's locals).
    auto d1 = deck1;
    auto d2 = deck2;
    StepDriver* driver = step_driver_.get();
    driver->runInFiber([this, d1, d2, seed, driver]() {
        step_result_ = runGame(d1, d2, *driver, *driver, seed);
        driver->markDone();
    });

    // No-op for fiber backend (selectAction's yield already returned
    // us here). Kept for API symmetry with the old thread backend.
    step_driver_->waitForDecision();
    refreshStepFromDriver();
    return current_step_;
}

bool isCleanResumePoint(const GameState& state) {
    if (state.game_over ||
        !state.turn.awaiting_main_action ||
        state.turn.phase != TurnPhase::MainPhase ||
        state.turn.oc_state != OpenClosedState::Open ||
        state.turn.ns_state != NeutralShowdownState::Neutral ||
        state.chain.exists() ||
        state.cost_cursor.has_value())
        return false;
    for (const auto& bf : state.battlefields) {
        if (bf.combat_in_progress || bf.showdown_in_progress ||
            bf.combat_staged || bf.showdown_staged)
            return false;
    }
    return true;
}

bool isCleanShowdownPoint(const GameState& state) {
    if (state.game_over ||
        !state.turn.awaiting_focus_at.has_value() ||
        !state.turn.focus_holder.has_value() ||
        state.turn.phase != TurnPhase::MainPhase ||
        state.turn.oc_state != OpenClosedState::Open ||
        state.chain.exists() ||
        state.cost_cursor.has_value())
        return false;
    for (const auto& bf : state.battlefields) {
        if (bf.id != *state.turn.awaiting_focus_at) continue;
        return bf.showdown_in_progress ||
               (bf.combat_in_progress && bf.combat_phase == CombatPhase::ShowdownStep);
    }
    return false;
}

bool isCleanChainPoint(const GameState& state) {
    if (state.game_over ||
        !state.chain.awaiting_priority ||
        !state.chain.exists() ||
        state.chain.resuming.has_value() ||
        state.chain.hasPending() ||
        !state.turn.priority_holder.has_value() ||
        state.cost_cursor.has_value())
        return false;
    const TurnPhase phase = state.turn.phase;
    auto step_bf = [&]() -> const BattlefieldState* {
        if (!state.turn.chain_step_bf.has_value()) return nullptr;
        for (const auto& bf : state.battlefields)
            if (bf.id == *state.turn.chain_step_bf) return &bf;
        return nullptr;
    };
    switch (state.turn.chain_step) {
        case ChainStep::MainAction:
        case ChainStep::MainActionCleanedUp:
        case ChainStep::StagedRound:
            return phase == TurnPhase::MainPhase;
        case ChainStep::ShowdownAction: {
            const BattlefieldState* bf = step_bf();
            return phase == TurnPhase::MainPhase && bf && state.turn.focus_holder.has_value() &&
                   (bf->showdown_in_progress ||
                    (bf->combat_in_progress && bf->combat_phase == CombatPhase::ShowdownStep));
        }
        case ChainStep::CombatStart: {
            const BattlefieldState* bf = step_bf();
            return phase == TurnPhase::MainPhase && bf && bf->combat_in_progress &&
                   bf->attacker.has_value() && bf->defender.has_value();
        }
        case ChainStep::BeginningStep:
            return phase == TurnPhase::BeginningStep;
        case ChainStep::ScoringStep:
            return phase == TurnPhase::ScoringStep && step_bf();
        case ChainStep::EndingStep:
            return phase == TurnPhase::EndingStep;
        case ChainStep::ExpirationStep:
            return phase == TurnPhase::ExpirationStep;
        case ChainStep::None:
            break;
    }
    return false;
}

bool isCleanSetupPoint(const GameState& state) {
    return !state.game_over &&
           (state.turn.phase == TurnPhase::Setup || state.turn.phase == TurnPhase::Mulligan);
}

StepResult GameEngine::resumeFromSnapshot(GameState snapshot_state,
                                            uint64_t engine_seed) {
    if (step_driver_) {
        throw std::logic_error(
            "GameEngine::resumeFromSnapshot called on an engine that already "
            "has a step-machine session. Construct a fresh GameEngine.");
    }

    // Substitute the snapshotted state.
    state_ = std::move(snapshot_state);
    rng_.seed(engine_seed == 0 ? std::random_device{}() : engine_seed);

    // Initialise subsystems (parallel to runGame's first block). These
    // hold references into `state_` and `events_`, which are now
    // populated from the snapshot.
    initSubsystems();

    step_driver_ = std::make_unique<StepDriver>();
    step_result_ = GameResult{};
    StepDriver* driver = step_driver_.get();
    agents_[0] = driver;
    agents_[1] = driver;

    // Capture the resume point — the phase the snapshot was at. The
    // dispatcher below uses this to skip already-completed phases.
    TurnPhase resume_at = state_.turn.phase;
    PlayerId  turn_player = state_.turn.turn_player;

    driver->runInFiber([this, resume_at, turn_player, driver]() {
        // Finish the current turn from where it paused, then continue
        // the normal turn loop. Mirrors the body of runTurnLoop minus
        // the first-turn entry.
        try {
            if (resume_at == TurnPhase::Setup || resume_at == TurnPhase::Mulligan) {
                // The rest of runGame from the setup decision it paused at.
                if (resume_at == TurnPhase::Setup) {
                    pickBattlefields();
                    drawOpeningHands();
                }
                runMulligans();
                events_.emit(PhaseChangedEvent{TurnPhase::Setup, TurnPhase::AwakenPhase,
                                               state_.turn.turn_player});
                runTurnLoop();
            } else if (!state_.game_over) {
                runTurnFromPhase(turn_player, resume_at);
            }
            if (!state_.game_over && resume_at != TurnPhase::Setup &&
                resume_at != TurnPhase::Mulligan) {
                // Apply the same turn-transition logic runTurnLoop uses.
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
                    state_.turn.turn_player = opponent(state_.turn.turn_player);
                    state_.turn.is_additional_turn = false;
                }
                state_.turn.turn_number++;
                runTurnLoop();
            }

            // Finalise (mirrors runGame's tail).
            step_result_.winner          = state_.winner;
            step_result_.final_scores[0] = state_.players[0].score;
            step_result_.final_scores[1] = state_.players[1].score;
            step_result_.total_turns     = state_.turn.turn_number;
            step_result_.total_decisions = state_.decision_index;
            step_result_.termination_reason = state_.game_over_reason;
        } catch (...) {
            // Defensive — must always reach markDone, otherwise the
            // caller hangs (fiber backend: it doesn't hang per se,
            // but isDone() never becomes true and the fiber leaks).
        }
        driver->markDone();
    });

    step_driver_->waitForDecision();
    refreshStepFromDriver();
    return current_step_;
}

StepResult GameEngine::currentStep() const {
    return current_step_;
}

StepResult GameEngine::applyChoice(int legal_index) {
    if (!step_driver_) {
        throw std::logic_error(
            "GameEngine::applyChoice called before beginGame.");
    }
    if (current_step_.kind == StepKind::Done) {
        return current_step_;
    }
    step_driver_->provideChoice(legal_index);
    step_driver_->waitForDecision();
    refreshStepFromDriver();
    return current_step_;
}

bool GameEngine::isStepDone() const {
    if (!step_driver_) return false;
    return step_driver_->isDone();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::setupGame(const DeckSubmission& deck1,
                            const DeckSubmission& deck2) {
    state_.turn.phase = TurnPhase::Setup;

    state_.players[0].id = PlayerId::Player1;
    state_.players[1].id = PlayerId::Player2;

    setupPlayer(PlayerId::Player1, deck1);
    setupPlayer(PlayerId::Player2, deck2);

    determineTurnOrder();
    setupBattlefields(deck1, deck2);
}

void GameEngine::setupPlayer(PlayerId player, const DeckSubmission& deck) {
    auto& ps = state_.player(player);

    // Legend → Legend Zone
    auto legend_id = instantiateCard(deck.legend, player);
    auto& legend_obj = state_.getObject(legend_id);
    legend_obj.zone = ZoneType::LegendZone;
    ps.legend_zone = legend_id;

    // Chosen Champion → Champion Zone (starts here, can be played from here)
    auto champ_id = instantiateCard(deck.chosen_champion, player);
    auto& champ_obj = state_.getObject(champ_id);
    champ_obj.zone = ZoneType::ChampionZone;
    // Champion zone has no board location — it's a non-board zone
    ps.champion_zone = champ_id;

    // Main Deck → shuffled
    for (auto card_def_id : deck.main_deck) {
        auto obj_id = instantiateCard(card_def_id, player);
        state_.getObject(obj_id).zone = ZoneType::MainDeck;
        ps.main_deck.push_back(obj_id);
    }
    shuffleDeck(player);

    // Rune Deck → shuffled
    for (auto rune_def_id : deck.rune_deck) {
        auto obj_id = instantiateCard(rune_def_id, player);
        state_.getObject(obj_id).zone = ZoneType::RuneDeck;
        ps.rune_deck.push_back(obj_id);
    }
    shuffleRuneDeck(player);

    // Legends chosen in addition to the starting legend (Neeko). Taken from
    // the deck list; any not listed are filled with the lowest-id legends that
    // share a domain with the starting legend. Different means no name match.
    int extra = 0;
    auto wants = [&](CardDefId id) {
        if (const Card* c = card_registry_.get(id))
            extra = std::max(extra, c->extraLegendsAtDeckBuilding());
    };
    wants(deck.chosen_champion);
    for (auto id : deck.main_deck) wants(id);
    if (extra > 0) {
        const auto& start = card_db_.get(deck.legend);
        std::vector<std::string> names = {start.name};
        auto take = [&](CardDefId id) {
            if (static_cast<int>(ps.reserve_legends.size()) >= extra) return;
            const Card* c = card_registry_.get(id);
            if (!c || c->def().card_type != CardType::Legend) return;
            if (std::find(names.begin(), names.end(), c->def().name) != names.end()) return;
            names.push_back(c->def().name);
            ps.reserve_legends.push_back(id);
        };
        for (auto id : deck.extra_legends) take(id);
        for (CardDefId id : card_registry_.classDefIds()) {
            const Card* c = card_registry_.get(id);
            if (!c || c->def().card_type != CardType::Legend || c->def().banned) continue;
            bool shares = false;
            for (auto d : c->def().domains)
                for (auto sd : start.domains) shares = shares || d == sd;
            if (shares) take(id);
        }
    }
}


void GameEngine::setupBattlefields(const DeckSubmission& deck1,
                                     const DeckSubmission& deck2) {
    // Each player picks one of their three battlefields (CR 480.5 says
    // random for single-game, CR 481.5 says agent choice for match
    // play). We unconditionally route through the agent — RandomAgent
    // picks uniformly which satisfies CR 480.5; HumanAgent / MCTS get
    // to actually choose, which CR 481.5 requires for match play.
    //
    // The Intent::chosen_battlefield field carries the INDEX into the
    // deck's battlefield list (0..N-1) here, NOT a runtime BattlefieldId
    // — no live BFs exist on the board yet at this point. Action-vocab
    // encoding via bfSlot handles 0..2 (well within its 0..7 range).
    //
    // The candidate pools go on PlayerState so the legal-action renderer
    // can label each ChooseBattlefield by its printed name ("Vilemaw's
    // Lair", "Rockfall Path", …) instead of just "BF #N", and so a
    // snapshot taken at a pick can finish the setup (pickBattlefields).
    state_.player(PlayerId::Player1).battlefield_pool = deck1.battlefields;
    state_.player(PlayerId::Player2).battlefield_pool = deck2.battlefields;
    state_.turn.setup_step = 0;
    pickBattlefields();
}

void GameEngine::pickBattlefields() {
    auto pickFromDeck = [&](PlayerId player) -> CardDefId {
        const auto& bfs = state_.player(player).battlefield_pool;
        if (bfs.empty()) return kInvalidId;
        // Do NOT short-circuit on bfs.size() == 1. Per the project
        // contract, every engine-asked decision MUST be surfaced to
        // the agent — including forced single-option ones — so that
        // training data, action_history replay, and the policy head's
        // exposure to "constrained" positions stay consistent.
        // The decision still costs nothing for a 1-option pool
        // (HumanAgent / RandomAgent / MCTSAgent each immediately
        // return the only entry), but the position is recorded.

        std::vector<Intent> choices;
        choices.reserve(bfs.size());
        for (size_t i = 0; i < bfs.size(); ++i) {
            Intent c;
            c.type = IntentType::ChooseBattlefield;
            c.player = player;
            c.chosen_battlefield = static_cast<BattlefieldId>(i);  // deck index
            choices.push_back(c);
        }

        // Briefly mark this player as the decision-maker so HumanAgent's
        // UI snapshot (which reads state.turn.turn_player) shows the
        // correct seat. The real turn order (starting_player) isn't owned
        // by this setup step; it is restored after the picks.
        state_.turn.turn_player = player;
        state_.decision_index++;
        events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                          " (" + toString(player) + " — battlefield setup): " +
                          std::to_string(choices.size()) + " options");
        auto chosen = getAgent(player).selectAction(state_, choices);
        recordAppliedIntent(chosen);

        size_t idx = static_cast<size_t>(chosen.chosen_battlefield);
        if (idx >= bfs.size()) idx = 0;  // defensive — fall back to first
        events_.logTrace("  CHOSE BF #" + std::to_string(idx) + " (" +
                          card_db_.get(bfs[idx]).name + ")");
        return bfs[idx];
    };

    for (int k = state_.turn.setup_step; k < 2; ++k) {
        state_.turn.setup_step = k;
        const PlayerId player = k == 0 ? PlayerId::Player1 : PlayerId::Player2;
        state_.player(player).chosen_battlefield = pickFromDeck(player);
    }
    state_.turn.setup_step = 0;
    state_.turn.turn_player = state_.turn.starting_player;
    auto bf1_def = state_.player(PlayerId::Player1).chosen_battlefield;
    auto bf2_def = state_.player(PlayerId::Player2).chosen_battlefield;

    // Create battlefield game objects and state
    BattlefieldId next_bf_id = 0;

    auto addBF = [&](CardDefId def_id, PlayerId contributor) {
        auto obj_id = instantiateCard(def_id, contributor);
        auto& obj = state_.getObject(obj_id);
        obj.zone = ZoneType::BattlefieldZone;

        BattlefieldState bf;
        bf.id = next_bf_id++;
        bf.card_object_id = obj_id;
        bf.contributed_by = contributor;

        // Parse static battlefield restrictions from ability text. These
        // flags are consulted by the action generators (move + play)
        // without needing per-BF Card dispatch on every legal-action
        // check.
        if (def_id != kInvalidId) {
            const auto& def = card_db_.get(def_id);
            std::string t = def.ability_text;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t.find("can't move from here to base") != std::string::npos)
                bf.blocks_move_to_base = true;
            if (t.find("can't be played here") != std::string::npos)
                bf.blocks_unit_play = true;
        }
        // Turn-gated scoring is a structured property the BF card decides
        // (Forgotten Monument). Engine asks; card answers.
        if (auto* bfc = dynamic_cast<const BattlefieldCard*>(
                card_registry_.get(def_id))) {
            bf.min_turn_to_score = bfc->minTurnToScore();
        }
        state_.battlefields.push_back(bf);
    };

    addBF(bf1_def, PlayerId::Player1);
    addBF(bf2_def, PlayerId::Player2);
}

void GameEngine::determineTurnOrder() {
    // Random coin flip for first player (CR 116)
    std::uniform_int_distribution<int> coin(0, 1);
    state_.turn.turn_player = coin(rng_) == 0 ? PlayerId::Player1 : PlayerId::Player2;
    state_.turn.starting_player = state_.turn.turn_player;
    state_.turn.turn_number = 0;

    // Mark both players as being on their first turn
    state_.players[0].is_first_turn = true;
    state_.players[1].is_first_turn = true;
}

void GameEngine::drawOpeningHands() {
    // Each player draws 4 (CR 117)
    events_.logTrace("── Opening Hands ──");
    events_.logTrace(std::string("DRAW_HAND: ") + toString(PlayerId::Player1) + " draws 4");
    drawCards(PlayerId::Player1, 4);
    events_.logTrace(std::string("DRAW_HAND: ") + toString(PlayerId::Player2) + " draws 4");
    drawCards(PlayerId::Player2, 4);
}

// C-1 commit 8 — mulligan loop split into advance/resolve halves.
GameEngine::MulliganAdvance GameEngine::advanceMulligan(PlayerId player) {
    MulliganAdvance adv;
    adv.legal = generateMulliganActions(player);
    if (adv.legal.empty()) {
        adv.kind = MulliganAdvance::Kind::Done;
        return adv;
    }
    adv.kind = MulliganAdvance::Kind::NeedDecision;
    adv.deciding = player;
    return adv;
}

void GameEngine::resolveMulliganDecision(const Intent& chosen) {
    // Log what was chosen. Including the player tag inline so each
    // CHOSE: line is self-contained — important for the web UI's log
    // panel, which prepends events (latest first), so a CHOSE line
    // can be visually separated from its DECISION # context.
    std::string who = std::string("[") + toString(chosen.player) + "] ";
    if (chosen.cards_to_mulligan.empty()) {
        events_.logTrace("CHOSE: " + who + "Keep hand");
    } else {
        std::string mull_str;
        for (auto cid : chosen.cards_to_mulligan) {
            if (!mull_str.empty()) mull_str += ", ";
            if (state_.objectExists(cid)) mull_str += state_.getObject(cid).name;
        }
        events_.logTrace("CHOSE: " + who + "Mulligan " +
                         std::to_string(chosen.cards_to_mulligan.size()) +
                         " [" + mull_str + "]");
    }
    if (chosen.type == IntentType::MulliganDecision) {
        executeMulligan(chosen);
    }
}

void GameEngine::runMulligans() {
    // In turn order, each player may mulligan up to 2 cards (CR 118).
    // Bridge: legacy path calls queryAgent between advance + resolve.
    // The step-machine path (C-1 commit 9) will surface advance's legal
    // options out to applyChoice() and feed the chosen Intent back into
    // resolve.
    events_.logTrace("── Mulligan Phase ──");
    state_.turn.phase = TurnPhase::Mulligan;

    for (int k = state_.turn.setup_step; k < 2; ++k) {
        state_.turn.setup_step = k;
        const PlayerId player = k == 0 ? state_.turn.turn_player
                                       : opponent(state_.turn.turn_player);
        auto adv = advanceMulligan(player);
        if (adv.kind == MulliganAdvance::Kind::Done) continue;

        state_.decision_index++;
        if (player == PlayerId::Player1) state_.turn.turn_decisions_p1++;
        else state_.turn.turn_decisions_p2++;

        events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                         " (" + toString(player) + "): Mulligan, " +
                         std::to_string(adv.legal.size()) + " options");

        auto chosen = getAgent(player).selectAction(state_, adv.legal);
        recordAppliedIntent(chosen);

        if (on_decision) {
            on_decision(state_, adv.legal, chosen);
        }

        resolveMulliganDecision(chosen);
    }
    state_.turn.setup_step = 0;
}

} // namespace riftbound
