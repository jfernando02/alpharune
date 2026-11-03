#pragma once
/// @file heuristic_agent.h
/// One-turn look-ahead agent scored by a hand-written evaluation.
///
/// At a decision where the engine can be restarted from a copy of the state
/// (`isLookaheadPoint`: main-phase priority, nothing on the chain, no
/// combat), the agent tries every legal action:
///
///   for each of `guesses` determinizations of the hidden cards
///       for each legal action
///           copy the guessed state, restart an engine on it, apply the
///           action, let ScriptedAgent play both seats until the turn ends
///           (or the opponent's next turn, with turns_ahead = 2), and score
///           the result with `evaluate`
///
/// and picks the action with the best average. All actions share the same
/// guesses, so their scores differ because of the action, not because one
/// was lucky. Ties go to the earlier legal action.
///
/// Everywhere else (reactions, targets, combat damage, rune payment,
/// mulligans) it answers as ScriptedAgent does. Looking ahead there would
/// need the replay path, which restores the real hidden cards.

#include "agent_interface.h"
#include "evaluator.h"
#include "scripted_agent.h"

#include "cards/card_registry.h"
#include "core/card_db.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <random>

namespace riftbound {

struct HeuristicConfig {
    int guesses = 4;                // determinizations per decision
    int max_rollout_steps = 400;    // decisions simulated per action, at most
    // Turns each simulation plays before scoring: 1 stops at the end of the
    // agent's turn; 2 also plays the opponent's next turn, which is where
    // Energy held for a [Reaction] / [Action] card pays off (or is wasted).
    int turns_ahead = 1;
    // Searched decisions per turn before the agent ends its turn. A legal
    // action that changes nothing (an engine bug) would otherwise be
    // re-chosen forever, since the rollout after it keeps playing.
    int max_lookaheads_per_turn = 100;
    // Rollout steps (decisions simulated, across every simulation) the agent
    // may spend searching one turn; 0 = no limit. A search costs legal actions
    // x guesses x rollout length, and all three grow with the board: a deck
    // that floods it with tokens (Eye of the Herald's Recruits) reached 54
    // units, ~106 legal actions and hours per game. A decision that cannot
    // afford every guess searches with fewer; once the budget is spent the
    // agent plays the rest of the turn as ScriptedAgent. The budget is checked
    // between decisions, so one search may overshoot it. Over 44 arena games
    // (2026-10) ordinary turns peaked at ~2k steps (median game) and ~19k
    // (worst); the runaway turns spent 175k-970k before this cap.
    long max_rollout_steps_per_turn = 40000;
    // Also search Showdown focus decisions (play an Action / Reaction or
    // pass) — 14% of real decisions in heuristic games, otherwise answered
    // as ScriptedAgent — not only the main-phase priority decision. Against
    // the same agent without it (2026-11, 600 games each, sign test on
    // decisive pairs): 65-19 and 76-20 with mirrored decks, 38-20 with mixed
    // ones (p from 1e-9 to 0.01), for ~13% more thinking time.
    bool search_showdowns = true;
    EvalWeights weights;
    // When set, positions are scored by this model's win probability
    // instead of `evaluate(weights)`.
    std::shared_ptr<const PositionModel> model;
    uint64_t seed = 1;              // 0 picks a random seed
};

class HeuristicAgent : public AgentInterface {
public:
    struct Stats {
        int decisions = 0;           // selectAction calls with 2+ legal actions
        int lookahead_decisions = 0; // ... of which were searched
        int lookahead_end_turns = 0; // ... where the search chose EndTurn
        int simulations = 0;         // simulations that produced a score
        int failed_simulations = 0;  // threw, or could not match the action
        int turn_cap_hits = 0;       // turns ended by max_lookaheads_per_turn
        long rollout_steps = 0;      // decisions simulated, across all simulations
        int thinned_searches = 0;    // searched with fewer guesses to fit the budget
        int budget_fallbacks = 0;    // answered as ScriptedAgent: turn budget spent
        int showdown_searches = 0;   // searched decisions that were Showdown focus
        // Searched EndTurn picks with a ready rune and a card still playable
        // ("left Energy unspent"), and how many of those held an [Action] or
        // [Reaction] card in hand.
        int end_turn_unspent = 0;
        int end_turn_unspent_holding_instant = 0;
    };

    HeuristicAgent(const CardDB& card_db, const CardRegistry& card_registry,
                   HeuristicConfig config = {});

    Intent selectAction(const GameState& state,
                        const std::vector<Intent>& legal_actions) override;

    const Stats& stats() const { return stats_; }

    /// True when a resumed copy of `state` reproduces this decision, so the
    /// agent may search it: `isCleanResumePoint` (the main-phase priority
    /// decision) with EndTurn on offer, which the turn cap falls back to.
    static bool isLookaheadPoint(const GameState& state,
                                 const std::vector<Intent>& legal_actions);
    /// The same for a Showdown focus decision (`isCleanShowdownPoint`, with
    /// PassFocus on offer), searched when `search_showdowns` is set.
    static bool isShowdownLookaheadPoint(const GameState& state,
                                         const std::vector<Intent>& legal_actions);

private:
    /// Score of playing `action` from the already-determinized `guess`, or
    /// nullopt when the simulation could not run.
    std::optional<double> simulate(const GameState& guess, const Intent& action,
                                   PlayerId me, uint64_t engine_seed);
    /// Stats for an EndTurn pick: was Energy left that could have been spent?
    void countUnspentEnergy(const GameState& state, const std::vector<Intent>& legal,
                            PlayerId me);

    const CardDB& card_db_;
    const CardRegistry& card_registry_;
    HeuristicConfig config_;
    ScriptedAgent scripted_;
    std::mt19937_64 rng_;
    Stats stats_;
    int lookahead_turn_ = -1;        // turn_number the counts below are for
    int lookaheads_this_turn_ = 0;
    long steps_this_turn_ = 0;       // rollout steps spent searching this turn
};

} // namespace riftbound
