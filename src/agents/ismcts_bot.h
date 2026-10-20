#pragma once
/// @file ismcts_bot.h
/// Information Set MCTS (Cowling et al. '12) on the engine's step API,
/// following OpenSpiel's `algorithms/is_mcts.{h,cc}` (ISMCTSBot) as closely
/// as the engine allows. Same structure and names: nodes are looked up by
/// (player, information-state key) in a hash map, a new node is evaluated
/// once and returns, CheckExpand picks an untried action from a shuffled
/// legal list, UCB uses the node's total visits with random tie-breaking,
/// visits are counted before recursing, FilterIllegals handles action sets
/// that differ between determinizations, and the final policy is one of
/// normalized visit count / max visit count / max value.
///
/// Where it differs, and why:
///   - Root sampling is `determinize` + `GameEngine::resumeFromSnapshot`.
///     OpenSpiel's RiftboundState clones by replaying history, which would
///     restore the real hidden cards.
///   - Actions are `Intent`s compared by equality, not vocab ids, so two
///     legal actions never share an edge (see tools/collision_census).
///   - A decision with one legal action is applied without a node; the
///     engine has many forced passes and each would cost a simulation.
///   - A simulation stops after `max_steps` decisions and evaluates there,
///     a guard against a legal action that changes nothing.
///   - The evaluator plays ScriptedAgent to the horizon (`turns_ahead`
///     turns from the root) and returns tanh(evaluate / value_scale) for
///     each player; past the horizon it evaluates the state as it is.
///   - Search runs only where a resumed engine reproduces the decision
///     (HeuristicAgent::isLookaheadPoint); elsewhere it plays ScriptedAgent.
/// The key is the hash RiftboundState::ObservationString uses: masked
/// state features for the acting player (ml::computeInfoSetId).

#include "agent_interface.h"
#include "evaluator.h"
#include "scripted_agent.h"

#include "cards/card_registry.h"
#include "core/card_db.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace riftbound {

// Use this constant to use an unlimited number of world samples.
inline constexpr int kUnlimitedNumWorldSamples = -1;

enum class IsmctsFinalPolicyType {
    kNormalizedVisitCount,  // sample in proportion to root visit counts
    kMaxVisitCount,
    kMaxValue,
};

struct IsmctsConfig {
    double uct_c = 2.0;              // as the engine's OpenSpiel ISMCTS agent
    int max_simulations = 300;
    int max_world_samples = kUnlimitedNumWorldSamples;
    IsmctsFinalPolicyType final_policy_type = IsmctsFinalPolicyType::kMaxVisitCount;
    // Determinizations make the same key show different legal actions, so
    // this is on (OpenSpiel's default constructor has it off).
    bool allow_inconsistent_action_sets = true;
    // Key every node by the searcher's view, as single-observer IS-MCTS in
    // Cowling et al. does. OpenSpiel keys a node by the acting player's
    // view; at the opponent's decisions that view holds their (guessed)
    // hand, which differs in nearly every determinization here, so opponent
    // nodes were never shared: 2/3 of simulations ended at a fresh opponent
    // node and the tree barely grew past the root.
    bool key_by_searcher = true;
    // Treat every state past the horizon as a leaf, so all lines are scored
    // at the same point (the first decision after the horizon's last turn).
    // OpenSpiel's tree has no horizon; here, with a playout that stops at
    // the end of the turn, a line that ended the turn early went on into
    // the opponent's turn inside the tree and was scored at a different
    // point than a line that took more actions.
    bool cut_tree_at_horizon = true;
    // Tree decisions per simulation before the evaluator takes over; 0 is
    // unlimited (OpenSpiel). 1 searches the root's actions only, the
    // HeuristicAgent's shape, which isolates bias from the deeper tree.
    int max_tree_depth = 0;

    // Evaluator.
    bool rollout = true;             // play ScriptedAgent to the horizon first
    int turns_ahead = 1;             // horizon, counted from the root's turn
    double value_scale = 150.0;      // a point is 100 in evaluate()
    EvalWeights weights;
    int max_steps = 400;             // decisions per simulation, tree + playout

    int max_lookaheads_per_turn = 100;  // see HeuristicConfig
    uint64_t seed = 1;               // 0 picks a random seed
};

class IsmctsBot : public AgentInterface {
public:
    struct Stats {
        int decisions = 0;           // selectAction calls with 2+ legal actions
        int searched_decisions = 0;  // ... of which were searched
        int simulations = 0;
        int failed_simulations = 0;  // threw, or the sampled root did not match
        int root_key_mismatches = 0;    // ... of which: infostate key differed
        int root_legal_mismatches = 0;  // ... of which: legal actions differed
        int turn_cap_hits = 0;
        int max_nodes = 0;           // largest tree, over all searches
        long tree_steps = 0;         // tree-policy decisions, summed over simulations
        int leaves_own = 0;          // simulations that left the tree at `me`'s decision
        int leaves_opponent = 0;     // ... at the opponent's decision
        int leaves_other = 0;        // ... at a terminal state or the step guard
    };

    IsmctsBot(const CardDB& card_db, const CardRegistry& card_registry,
              IsmctsConfig config = {});
    ~IsmctsBot() override;

    Intent selectAction(const GameState& state,
                        const std::vector<Intent>& legal_actions) override;

    /// The search's policy over `legal_actions` (zero for actions it never
    /// tried). Requires a state where isLookaheadPoint holds.
    std::vector<std::pair<Intent, double>> RunSearch(const GameState& state,
                                                     const std::vector<Intent>& legal_actions);

    const Stats& stats() const { return stats_; }

private:
    using StateKey = std::pair<int, uint64_t>;  // (player index, infostate hash)
    struct StateKeyHash {
        size_t operator()(const StateKey& k) const {
            return std::hash<uint64_t>()(k.second) ^ static_cast<size_t>(k.first);
        }
    };
    struct ChildInfo {
        Intent action;
        int visits = 0;
        double return_sum = 0.0;
        double value() const { return return_sum / visits; }
    };
    struct Node {
        std::vector<ChildInfo> child_info;
        int total_visits = 0;
        ChildInfo* find(const Intent& action);
    };
    struct Sim;  // a live engine on one sampled world

    void Reset();
    double RandomNumber();
    StateKey GetStateKey(const GameState& state, PlayerId player) const;
    std::unique_ptr<Sim> SampleRootState(const GameState& state, PlayerId me);
    Node* CreateNewNode(const StateKey& key);
    Node* LookupNode(const StateKey& key);
    Node* LookupOrCreateNode(const StateKey& key);
    Intent SelectActionTreePolicy(Node* node, const std::vector<Intent>& legal);
    Intent SelectActionUCB(Node* node);
    std::vector<std::pair<Intent, double>> GetFinalPolicy(
        const std::vector<Intent>& legal, Node* node) const;
    void ExpandIfNecessary(Node* node, const Intent& action) const;
    /// An action of `legal` not yet in the node's children, or nullopt.
    std::optional<Intent> CheckExpand(Node* node, const std::vector<Intent>& legal);
    Node FilterIllegals(Node* node, const std::vector<Intent>& legal) const;
    /// Returns for (Player1, Player2).
    std::vector<double> RunSimulation(Sim& sim, int depth = 0);
    std::vector<double> Evaluate(Sim& sim);

    const CardDB& card_db_;
    const CardRegistry& card_registry_;
    IsmctsConfig config_;
    ScriptedAgent scripted_;
    std::mt19937_64 rng_;
    std::unordered_map<StateKey, Node*, StateKeyHash> nodes_;
    std::vector<std::unique_ptr<Node>> node_pool_;
    std::vector<GameState> root_samples_;
    Node* root_node_ = nullptr;
    PlayerId searcher_ = PlayerId::None;
    int horizon_turn_ = 0;           // last turn number inside the horizon
    Stats stats_;
    int lookahead_turn_ = -1;
    int lookaheads_this_turn_ = 0;
};

} // namespace riftbound
