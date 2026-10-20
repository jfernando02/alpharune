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
    int lookahead_turn_ = -1;        // turn_number the count below is for
    int lookaheads_this_turn_ = 0;
};

} // namespace riftbound
