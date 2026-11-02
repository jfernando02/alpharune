#include "ismcts_bot.h"

#include "determinize.h"
#include "heuristic_agent.h"

#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/cfr_util.h"
#include "ml/feature_extractor.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace riftbound {

constexpr double kTieTolerance = 0.00001;
constexpr int kUnexpandedVisitCount = -1;

/// One sampled world: a live engine paused at a decision.
struct IsmctsBot::Sim {
    EventBus bus;  // declared first: outlives the engine
    GameEngine engine;
    StepResult step;
    int steps = 0;

    Sim(const CardDB& db, const CardRegistry& reg) : engine(db, bus, reg) {}

    void apply(int legal_index) {
        step = engine.applyChoice(legal_index);
        ++steps;
    }
};

IsmctsBot::ChildInfo* IsmctsBot::Node::find(const Intent& action) {
    for (auto& c : child_info)
        if (c.action == action) return &c;
    return nullptr;
}

IsmctsBot::IsmctsBot(const CardDB& card_db, const CardRegistry& card_registry,
                     IsmctsConfig config)
    : card_db_(card_db),
      card_registry_(card_registry),
      config_(config),
      scripted_(card_db),
      rng_(config.seed == 0 ? std::random_device{}() : config.seed) {}

IsmctsBot::~IsmctsBot() = default;

double IsmctsBot::RandomNumber() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

void IsmctsBot::Reset() {
    nodes_.clear();
    node_pool_.clear();
    root_samples_.clear();
}

IsmctsBot::StateKey IsmctsBot::GetStateKey(const GameState& state, PlayerId player) const {
    // The key's player is the one to act; the view is theirs, or the
    // searcher's when key_by_searcher is set.
    const PlayerId observer =
        (config_.key_by_searcher && searcher_ != PlayerId::None) ? searcher_ : player;
    const int p = playerIndex(player);
    return {p, ml::computeInfoSetId(ml::extractStateFeatures(state, observer, card_db_), p)};
}

Intent IsmctsBot::selectAction(const GameState& state, const std::vector<Intent>& legal) {
    if (legal.empty()) return Intent::concede(state.turn.turn_player);
    if (legal.size() == 1) return legal[0];
    ++stats_.decisions;

    if (!HeuristicAgent::isLookaheadPoint(state, legal))
        return scripted_.selectAction(state, legal);
    ++stats_.searched_decisions;

    if (state.turn.turn_number != lookahead_turn_) {
        lookahead_turn_ = state.turn.turn_number;
        lookaheads_this_turn_ = 0;
        steps_this_turn_ = 0;
    }
    if (config_.max_steps_per_turn > 0 && steps_this_turn_ >= config_.max_steps_per_turn) {
        ++stats_.budget_fallbacks;
        return scripted_.selectAction(state, legal);
    }
    if (++lookaheads_this_turn_ > config_.max_lookaheads_per_turn) {
        // isLookaheadPoint guarantees an EndTurn is present.
        ++stats_.turn_cap_hits;
        return *std::find_if(legal.begin(), legal.end(), [](const Intent& a) {
            return a.type == IntentType::EndTurn;
        });
    }

    // Step: sample from the final policy (a point mass unless the policy is
    // kNormalizedVisitCount).
    const auto policy = RunSearch(state, legal);
    double r = RandomNumber(), cumulative = 0.0;
    for (const auto& [action, prob] : policy) {
        cumulative += prob;
        if (r < cumulative) return action;
    }
    return scripted_.selectAction(state, legal);  // empty search
}

std::vector<std::pair<Intent, double>> IsmctsBot::RunSearch(const GameState& state,
                                                            const std::vector<Intent>& legal) {
    Reset();
    if (legal.size() == 1) return {{legal[0], 1.0}};

    const PlayerId me = legal[0].player;
    searcher_ = me;
    horizon_turn_ = state.turn.turn_number + config_.turns_ahead - 1;
    // The root key comes from a resumed copy of the real state, because the
    // sampled roots are resumed too: resuming re-runs cleanup, which can
    // clear a stale flag the live state still carries (a battlefield left
    // Contested by a player with no units there, CR 323.10).
    StateKey root_key;
    {
        Sim real(card_db_, card_registry_);
        real.step = real.engine.resumeFromSnapshot(state, rng_() | 1);
        root_key = GetStateKey(real.engine.state(), me);
    }
    root_node_ = CreateNewNode(root_key);

    for (int sim = 0; sim < config_.max_simulations; ++sim) {
        if (config_.max_steps_per_turn > 0 && steps_this_turn_ >= config_.max_steps_per_turn) {
            ++stats_.budget_cut_searches;
            break;
        }
        try {
            std::unique_ptr<Sim> sampled_root = SampleRootState(state, me);
            // OpenSpiel checks the sampled root's key; the legal list must
            // match too, since actions are looked up in it.
            if (!sampled_root) {
                ++stats_.failed_simulations;
                continue;
            }
            if (GetStateKey(sampled_root->engine.state(), me) != root_key) {
                ++stats_.failed_simulations;
                ++stats_.root_key_mismatches;
                continue;
            }
            if (sampled_root->step.legal != legal) {
                ++stats_.failed_simulations;
                ++stats_.root_legal_mismatches;
                continue;
            }
            RunSimulation(*sampled_root);
            stats_.sim_steps += sampled_root->steps;
            steps_this_turn_ += sampled_root->steps;
            ++stats_.simulations;
        } catch (const std::exception&) {
            ++stats_.failed_simulations;
        }
    }
    stats_.max_nodes = std::max(stats_.max_nodes, static_cast<int>(node_pool_.size()));

    if (root_node_->total_visits <= 0) return {};
    if (config_.allow_inconsistent_action_sets) {
        Node temp_node = FilterIllegals(root_node_, legal);
        if (temp_node.total_visits <= 0) return {};
        return GetFinalPolicy(legal, &temp_node);
    }
    return GetFinalPolicy(legal, root_node_);
}

std::vector<std::pair<Intent, double>> IsmctsBot::GetFinalPolicy(
    const std::vector<Intent>& legal, Node* node) const {
    std::vector<std::pair<Intent, double>> policy;
    switch (config_.final_policy_type) {
        case IsmctsFinalPolicyType::kNormalizedVisitCount: {
            const double total_visits = static_cast<double>(node->total_visits);
            for (const auto& c : node->child_info)
                policy.push_back({c.action, c.visits / total_visits});
        } break;

        case IsmctsFinalPolicyType::kMaxVisitCount: {
            const ChildInfo* max_child = nullptr;
            for (const auto& c : node->child_info)
                if (!max_child || c.visits > max_child->visits) max_child = &c;
            for (const auto& c : node->child_info)
                policy.push_back({c.action, &c == max_child ? 1.0 : 0.0});
        } break;

        case IsmctsFinalPolicyType::kMaxValue: {
            const ChildInfo* max_child = nullptr;
            for (const auto& c : node->child_info)
                if (c.visits > 0 && (!max_child || c.value() > max_child->value()))
                    max_child = &c;
            for (const auto& c : node->child_info)
                policy.push_back({c.action, &c == max_child ? 1.0 : 0.0});
        } break;
    }

    // In case the search didn't cover all the legal moves, at zero
    // probability for all the remaining actions.
    for (const Intent& action : legal) {
        const bool listed = std::any_of(policy.begin(), policy.end(),
                                        [&](const auto& p) { return p.first == action; });
        if (!listed) policy.push_back({action, 0.0});
    }
    return policy;
}

std::unique_ptr<IsmctsBot::Sim> IsmctsBot::SampleRootState(const GameState& state, PlayerId me) {
    // ResampleFromInfostate: a fresh determinization, or one of the stored
    // world samples when their number is restricted.
    const GameState* world = nullptr;
    GameState fresh;
    if (config_.max_world_samples == kUnlimitedNumWorldSamples ||
        static_cast<int>(root_samples_.size()) < config_.max_world_samples) {
        fresh = state;
        determinize(fresh, me, rng_);
        if (config_.max_world_samples != kUnlimitedNumWorldSamples) {
            root_samples_.push_back(fresh);
            world = &root_samples_.back();
        } else {
            world = &fresh;
        }
    } else {
        std::uniform_int_distribution<size_t> pick(0, root_samples_.size() - 1);
        world = &root_samples_[pick(rng_)];
    }

    auto sim = std::make_unique<Sim>(card_db_, card_registry_);
    // Nonzero: resumeFromSnapshot treats 0 as "seed from random_device".
    sim->step = sim->engine.resumeFromSnapshot(*world, rng_() | 1);
    if (sim->step.kind != StepKind::NeedDecision) return nullptr;
    return sim;
}

IsmctsBot::Node* IsmctsBot::CreateNewNode(const StateKey& key) {
    node_pool_.push_back(std::make_unique<Node>());
    Node* node = node_pool_.back().get();
    nodes_[key] = node;
    node->total_visits = kUnexpandedVisitCount;
    return node;
}

IsmctsBot::Node* IsmctsBot::LookupNode(const StateKey& key) {
    auto iter = nodes_.find(key);
    return iter == nodes_.end() ? nullptr : iter->second;
}

IsmctsBot::Node* IsmctsBot::LookupOrCreateNode(const StateKey& key) {
    Node* node = LookupNode(key);
    return node != nullptr ? node : CreateNewNode(key);
}

IsmctsBot::Node IsmctsBot::FilterIllegals(Node* node, const std::vector<Intent>& legal) const {
    Node new_node;
    new_node.total_visits = node->total_visits;
    for (const auto& c : node->child_info) {
        if (std::find(legal.begin(), legal.end(), c.action) == legal.end())
            new_node.total_visits -= c.visits;  // illegal action: drop it
        else
            new_node.child_info.push_back(c);
    }
    return new_node;
}

void IsmctsBot::ExpandIfNecessary(Node* node, const Intent& action) const {
    if (!node->find(action)) node->child_info.push_back(ChildInfo{action, 0, 0.0});
}

Intent IsmctsBot::SelectActionTreePolicy(Node* node, const std::vector<Intent>& legal) {
    // With inconsistent action sets the node may hold actions that are not
    // legal in this state, so they are removed first.
    if (config_.allow_inconsistent_action_sets) {
        Node temp_node = FilterIllegals(node, legal);
        if (temp_node.total_visits == 0) {
            // Filtered everything: a random action.
            std::uniform_int_distribution<size_t> pick(0, legal.size() - 1);
            const Intent& action = legal[pick(rng_)];
            ExpandIfNecessary(node, action);
            return action;
        }
        return SelectActionUCB(&temp_node);
    }
    return SelectActionUCB(node);
}

Intent IsmctsBot::SelectActionUCB(Node* node) {
    std::vector<std::pair<const ChildInfo*, double>> actions_and_values;
    double max_value = -std::numeric_limits<double>::infinity();
    for (const auto& c : node->child_info) {
        // Every child has a visit: it is created only when a simulation
        // takes it, which counts the visit at once.
        const double uct_val = c.value() + config_.uct_c *
            std::sqrt(std::log(static_cast<double>(node->total_visits)) / c.visits);
        actions_and_values.push_back({&c, uct_val});
        max_value = std::max(max_value, uct_val);
    }

    std::vector<const ChildInfo*> candidates;
    for (const auto& [c, v] : actions_and_values)
        if (v > max_value - kTieTolerance) candidates.push_back(c);
    if (candidates.size() == 1) return candidates[0]->action;
    std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
    return candidates[pick(rng_)]->action;
}

std::optional<Intent> IsmctsBot::CheckExpand(Node* node, const std::vector<Intent>& legal) {
    // Fast check in the common/default case.
    if (!config_.allow_inconsistent_action_sets && node->child_info.size() == legal.size())
        return std::nullopt;

    // Shuffle the legal actions to remove the bias from the move order.
    std::vector<Intent> legal_copy = legal;
    std::shuffle(legal_copy.begin(), legal_copy.end(), rng_);
    for (const Intent& action : legal_copy)
        if (!node->find(action)) return action;
    return std::nullopt;
}

std::vector<double> IsmctsBot::RunSimulation(Sim& sim, int depth) {
    // Forced decisions are applied without a node (see header).
    while (sim.step.kind == StepKind::NeedDecision && sim.step.legal.size() == 1 &&
           sim.steps < config_.max_steps)
        sim.apply(0);

    if (sim.step.kind != StepKind::NeedDecision || sim.steps >= config_.max_steps ||
        (config_.cut_tree_at_horizon && sim.engine.state().turn.turn_number > horizon_turn_) ||
        (config_.max_tree_depth > 0 && depth >= config_.max_tree_depth)) {
        ++stats_.leaves_other;
        return Evaluate(sim);  // terminal, step guard, horizon or depth limit
    }

    const std::vector<Intent> legal_actions = sim.step.legal;
    const PlayerId cur_player = sim.step.perspective;
    Node* node = LookupOrCreateNode(GetStateKey(sim.engine.state(), cur_player));

    if (node->total_visits == kUnexpandedVisitCount) {
        // Newly created node, so we've just stepped out of the tree.
        node->total_visits = 0;  // Expand the node.
        ++(cur_player == searcher_ ? stats_.leaves_own : stats_.leaves_opponent);
        return Evaluate(sim);
    }
    ++stats_.tree_steps;

    // Apply tree policy.
    Intent chosen_action;
    if (auto expand = CheckExpand(node, legal_actions)) {
        chosen_action = *expand;
        ExpandIfNecessary(node, chosen_action);
    } else {
        // No expansion, so use the tree policy to select.
        chosen_action = SelectActionTreePolicy(node, legal_actions);
    }

    // Visits are updated before the recursive call: a key can repeat within
    // one simulation (imperfect recall), and the UCB denominator must not
    // be 0 when it does.
    node->total_visits++;
    node->find(chosen_action)->visits++;

    const auto index = std::find(legal_actions.begin(), legal_actions.end(), chosen_action) -
                       legal_actions.begin();
    sim.apply(static_cast<int>(index));
    std::vector<double> returns = RunSimulation(sim, depth + 1);
    // Looked up again: the recursion may have grown child_info.
    node->find(chosen_action)->return_sum += returns[playerIndex(cur_player)];
    return returns;
}

std::vector<double> IsmctsBot::Evaluate(Sim& sim) {
    const GameState& state = sim.engine.state();
    if (config_.rollout) {
        while (sim.step.kind == StepKind::NeedDecision &&
               sim.engine.state().turn.turn_number <= horizon_turn_ &&
               sim.steps < config_.max_steps) {
            const int pick = scripted_.selectIndex(state, sim.step.legal);
            sim.apply(pick < 0 ? 0 : pick);
        }
    }
    double v;
    if (!config_.model) {
        v = std::tanh(evaluate(state, PlayerId::Player1, config_.weights) / config_.value_scale);
    } else if (state.game_over) {
        v = state.winner == PlayerId::Player1 ? 1.0 : state.winner == PlayerId::Player2 ? -1.0 : 0.0;
    } else {
        v = 2.0 * config_.model->winProbability(state, PlayerId::Player1) - 1.0;
    }
    return {v, -v};
}

} // namespace riftbound
