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
/// The same search covers, when configured, Showdown focus decisions, chain
/// priority decisions and the setup decisions (battlefield pick, mulligan),
/// and the follow-ups of a searched action (rune payment, targets, ...) are
/// planned with it. Everywhere else (targets of resolving effects, combat
/// damage, triggers outside those steps) it answers as ScriptedAgent does:
/// a resumed engine cannot re-enter those decisions.
///
/// A learned policy (`HeuristicConfig::policy`, trained on this agent's own
/// search results) can stand in for ScriptedAgent — in the simulations and
/// for the unsearched decisions — at the decision kinds it covers.
///
/// With `HeuristicConfig::tree` the same decisions are searched with a tree
/// instead (heuristic_tree.cpp), which spends its simulations where the
/// policy and the results so far point and branches on both players'
/// decisions inside the turn.

#include "agent_interface.h"
#include "evaluator.h"
#include "policy_model.h"
#include "scripted_agent.h"

#include "cards/card_registry.h"
#include "core/card_db.h"

#include <cstdint>
#include <functional>
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
    // Plan the follow-up decisions of an action with it: after a searched
    // action, up to this many of the agent's own next decisions that finish
    // it (rune payment, a target or mode, an optional cost, a play location)
    // are branched over, each candidate plan simulated whole, and the chosen
    // plan's follow-ups are played when the engine asks for them. 0 leaves
    // them to ScriptedAgent. 1 vs 0 (2026-11, 600 games each, Showdown search
    // on in both, decisive pairs): 63-36, 62-32 mirrored, 48-20 mixed decks
    // (p 0.004 to 0.0005) for ~2.4x the thinking time; 2 vs 0 was no better
    // than 1 (57-36) at ~4x.
    int plan_followups = 1;
    int max_followup_options = 6;   // options tried per follow-up decision
    int max_plans = 96;             // candidate plans per search, at most
    // Also search chain priority decisions — add a Reaction / ability to
    // the chain or pass — inside a main-phase action or a Showdown focus
    // action (isCleanChainPoint). The simulation plays to the end of the
    // turn the chain is in, as for the other decisions.
    bool search_chain = false;
    // Also search the setup decisions: the battlefield pick and the
    // mulligan. Their simulations play the game from setup to the end of
    // turn `setup_turns - 1` (turn 0 is the first player's first turn),
    // with `setup_guesses` determinizations: the hand drawn afterwards
    // varies far more than a mid-game turn does.
    bool search_battlefield = false;
    bool search_mulligan = false;
    int setup_guesses = 8;
    int setup_turns = 2;
    EvalWeights weights;
    // When set, positions are scored by this model's win probability
    // instead of `evaluate(weights)`.
    std::shared_ptr<const PositionModel> model;
    // Learned policy (agents/policy_model), trained to rank actions as this
    // agent's search does. When set it replaces ScriptedAgent at the
    // decision kinds it covers: in the simulations (`policy_rollouts`, both
    // seats) and for the agent's own decisions it does not search
    // (`policy_offsearch`).
    std::shared_ptr<const PolicyModel> policy;
    bool policy_rollouts = true;
    bool policy_offsearch = true;
    // Search the Main / Showdown / Chain decisions with a tree instead of
    // trying every plan `guesses` times (heuristic_tree.cpp). Each
    // simulation draws a new guess of the hidden cards and walks down from
    // the decision: at each node (any decision of either player with two or
    // more options, keyed by what the searcher can see) it takes the option
    // with the best value + tree_c * prior * sqrt(times available) /
    // (1 + visits), PUCT as in AlphaZero, values min-max normalized over the
    // tree as in MuZero, priors the policy's scores through a softmax at
    // `tree_temperature`. At the first decision not yet in the tree the
    // simulation plays on as in the flat search (the policy, both seats) to
    // the end of the horizon turn and scores it with `model`. The decision
    // goes to the most visited option; the agent's later decisions in the
    // same turn (follow-ups, chain answers) come from the tree when its node
    // has `tree_min_reuse` visits, and the tree is kept until the turn ends.
    bool tree = false;
    int tree_sims = 200;            // simulations per searched decision, at most
    int tree_sims_per_action = 8;   // ... and at most this many per legal action
    double tree_c = 1.25;
    double tree_temperature = 1.0;
    int tree_width = 8;             // options per node below the root (best priors)
    int tree_min_reuse = 4;
    // The root as Gumbel MuZero's sequential halving instead of PUCT: the
    // `tree_halving_m` options with the best priors each get an equal share
    // of the round's simulations, all options of a round on the same guesses
    // of the hidden cards and engine seeds (common random numbers), and the
    // worse half by log prior + (50 + max visits) * tree_halving_scale *
    // normalized mean is dropped after each round; the decision goes to the
    // last one left. PUCT picks the most visited option, and with ~100
    // simulations the visits follow the priors, not the values.
    bool tree_halving = false;
    int tree_halving_m = 16;
    double tree_halving_scale = 1.0;
    // Do not branch on rune payments inside the tree: take the default
    // answer (the policy's, or ScriptedAgent's) without a node. Which rune
    // pays rarely matters (2026-11: paying at random plays level with
    // ScriptedAgent), and every payment node between two real decisions
    // costs the tree a level of depth and its simulations.
    bool tree_skip_rune = false;
    // First-play urgency: the value an unvisited option gets in PUCT is the
    // node's mean so far minus this (normalized, floored at 0) instead of the
    // worst value seen. Below 0: off (the worst value, as MuZero).
    double tree_fpu = -1.0;
    // Root exploration: the root's priors become (1 - mix) * policy + mix /
    // options, as AlphaZero mixes in noise. Without it the policy's prior
    // decides which root options get simulated at all: on a Jax turn 1 the
    // policy put 0.76 on End Turn and Doran's Blade got 1 of 16 simulations.
    double tree_root_mix = 0.0;
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
        int chain_searches = 0;      // ... chain priority
        int setup_searches = 0;      // ... battlefield pick or mulligan
        int planned_followups = 0;   // follow-up decisions answered from a plan
        int abandoned_plans = 0;     // plans dropped: the engine asked something else
        // Searched EndTurn picks with a ready rune and a card still playable
        // ("left Energy unspent"), and how many of those held an [Action] or
        // [Reaction] card in hand.
        int end_turn_unspent = 0;
        int end_turn_unspent_holding_instant = 0;
        // Tree search (HeuristicConfig::tree).
        int tree_searches = 0;
        long tree_steps = 0;         // decisions taken inside the tree, all simulations
        int tree_answers = 0;        // unsearched decisions answered from the tree
        long tree_rune_steps = 0;    // tree_steps that were rune payments
        int max_tree_nodes = 0;
        double root_max_prior = 0;   // sum over tree searches of the root's largest prior
    };

    HeuristicAgent(const CardDB& card_db, const CardRegistry& card_registry,
                   HeuristicConfig config = {});
    ~HeuristicAgent() override;

    Intent selectAction(const GameState& state,
                        const std::vector<Intent>& legal_actions) override;

    const Stats& stats() const { return stats_; }

    /// The last tree decision's root, per legal action (diagnostics; valid
    /// inside the SearchObserver call): visits and prior.
    struct RootDetail {
        std::vector<int> visits;
        std::vector<double> priors;
    };
    const RootDetail& lastRoot() const { return last_root_; }

    /// Told of every searched decision, and of every follow-up decision
    /// answered from a plan, with the search's score of each legal action
    /// (the best plan's score starting with it; NaN where none was
    /// simulated). The state is the real one, before the action. Training
    /// data for the policy and value models (tools/value_data.cpp).
    using SearchObserver = std::function<void(const GameState&, const std::vector<Intent>&,
                                              const std::vector<double>&)>;
    void setSearchObserver(SearchObserver observer) { observer_ = std::move(observer); }

    /// True when a resumed copy of `state` reproduces this decision, so the
    /// agent may search it: `isCleanResumePoint` (the main-phase priority
    /// decision) with EndTurn on offer, which the turn cap falls back to.
    static bool isLookaheadPoint(const GameState& state,
                                 const std::vector<Intent>& legal_actions);
    /// The same for a Showdown focus decision (`isCleanShowdownPoint`, with
    /// PassFocus on offer), searched when `search_showdowns` is set.
    static bool isShowdownLookaheadPoint(const GameState& state,
                                         const std::vector<Intent>& legal_actions);
    /// The same for a chain priority decision (`isCleanChainPoint`, with
    /// PassPriority on offer), searched when `search_chain` is set.
    static bool isChainLookaheadPoint(const GameState& state,
                                      const std::vector<Intent>& legal_actions);
    /// The same for a battlefield pick or a mulligan (`isCleanSetupPoint`),
    /// searched when `search_battlefield` / `search_mulligan` is set.
    static bool isSetupLookaheadPoint(const GameState& state,
                                      const std::vector<Intent>& legal_actions);

private:
    enum class SearchKind : uint8_t { None, Main, Showdown, Chain, Setup };
    /// Which kind of searched decision this is, given the config; None for
    /// the decisions answered as ScriptedAgent (or from a plan).
    SearchKind searchKind(const GameState& state, const std::vector<Intent>& legal) const;
    /// The last turn a simulation from `state` plays before scoring.
    int lastTurn(const GameState& state) const;

    /// Score of playing `plan` from the already-determinized `guess`, or
    /// nullopt when the simulation could not run. plan[0] is played at the
    /// root; each later step at `me`'s next follow-up decision (see
    /// isFollowup), everything else as ScriptedAgent.
    std::optional<double> simulate(const GameState& guess, const std::vector<Intent>& plan,
                                   PlayerId me, uint64_t engine_seed);
    /// A decision of `me` that finishes the action before it: not one the
    /// agent searches from scratch, and with a real choice.
    bool isFollowup(const GameState& state, const std::vector<Intent>& legal, PlayerId me) const;
    /// Candidate plans starting with `action`: the action alone, or with each
    /// option of its follow-up decisions, up to plan_followups deep, found
    /// by playing them out in `guess`.
    void expandPlans(const GameState& guess, uint64_t engine_seed, PlayerId me,
                     std::vector<Intent> prefix, std::vector<std::vector<Intent>>& out);
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
    /// ScriptedAgent's answer, or the policy's where it covers the decision
    /// and `use_policy` is set.
    int defaultIndex(const GameState& state, const std::vector<Intent>& legal,
                     bool use_policy) const;
    Intent defaultAction(const GameState& state, const std::vector<Intent>& legal) const;

    std::vector<Intent> plan_;       // follow-ups still to play, in order
    // For each step of plan_: the score of each option searched there.
    std::vector<std::vector<std::pair<Intent, double>>> plan_scores_;
    SearchObserver observer_;
    RootDetail last_root_;

    // Tree search (heuristic_tree.cpp).
    struct Tree;
    std::unique_ptr<Tree> tree_;
    Intent treeSearch(const GameState& state, const std::vector<Intent>& legal, PlayerId me);
    /// The tree's answer to an unsearched decision of the agent's, if its
    /// node there was visited enough.
    std::optional<Intent> treeAnswer(const GameState& state, const std::vector<Intent>& legal);
    /// One simulation from `state`; false when it could not run.
    /// The root option to take by sequential halving (tree_halving), or -1.
    int treeHalving(const GameState& state, const std::vector<Intent>& legal, PlayerId me,
                    uint64_t root_key, const std::vector<double>& priors, int sims,
                    bool& budget_spent);
    bool treeSimulation(const GameState& state, PlayerId me, uint64_t root_key,
                        const std::vector<Intent>& root_legal,
                        const std::vector<double>& root_priors,
                        int forced = -1,     // root option (index in root_legal) to take
                        uint64_t seed = 0);  // guess + engine seed; 0: from rng_
    uint64_t treeKey(const GameState& state, const std::vector<Intent>& legal, PlayerId me) const;
    std::vector<double> treePriors(const GameState& state, const std::vector<Intent>& legal) const;
    /// The searcher's value of a simulation's end state, in [0, 1].
    double leafValue(const GameState& end, PlayerId me) const;
};

} // namespace riftbound
