/// @file heuristic_tree.cpp
/// HeuristicAgent's tree search (HeuristicConfig::tree): PUCT over nodes
/// keyed by what the searcher can see, priors from the learned policy,
/// leaves played out and scored as in the flat search.

#include "heuristic_tree.h"

#include "determinize.h"

#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/cfr_util.h"
#include "ml/feature_extractor.h"
#include "ml/policy_features.h"

#include <cstring>
#include <exception>

namespace riftbound {

namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    return h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

uint64_t hashLegal(const std::vector<Intent>& legal) {
    uint64_t h = legal.size();
    for (const Intent& a : legal) {
        h = mix(h, static_cast<uint64_t>(a.type));
        h = mix(h, a.card);
        h = mix(h, a.ability_source);
        h = mix(h, static_cast<uint64_t>(a.ability_index));
        for (GameObjectId t : a.targets) h = mix(h, t);
        for (GameObjectId o : a.chosen_objects) h = mix(h, o);
        for (GameObjectId u : a.units_to_move) h = mix(h, u);
        h = mix(h, a.chosen_value ? static_cast<uint64_t>(*a.chosen_value) + 1 : 0);
        h = mix(h, a.chosen_card_def);
    }
    return h;
}

int findAction(const std::vector<Intent>& legal, const Intent& action) {
    for (size_t j = 0; j < legal.size(); ++j)
        if (legal[j] == action) return static_cast<int>(j);
    return -1;
}

} // namespace

uint64_t HeuristicAgent::treeKey(const GameState& state, const std::vector<Intent>& legal,
                                 PlayerId me) const {
    const PlayerId actor = legal[0].player;
    // The searcher's view at every node, as IsmctsConfig::key_by_searcher:
    // the opponent's own view holds a guessed hand, different in nearly
    // every simulation, so their nodes would never be shared.
    uint64_t key = ml::computeInfoSetId(ml::extractStateFeatures(state, me, card_db_),
                                        playerIndex(actor));
    // The searcher's decisions are told apart by their options too: the
    // view of the board can repeat within one action (a rune payment).
    if (actor == me) key = mix(key, hashLegal(legal));
    return key;
}

std::vector<double> HeuristicAgent::treePriors(const GameState& state,
                                               const std::vector<Intent>& legal) const {
    std::vector<double> p(legal.size(), 0.0);
    const int scripted = scripted_.selectIndex(state, legal);
    const char* kind = ml::policyDecisionKind(state, legal);
    if (config_.policy && config_.policy->covers(kind)) {
        const std::vector<double> s = config_.policy->scores(state, card_db_, legal, kind, scripted);
        double top = -std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < legal.size(); ++i)
            if (legal[i].type != IntentType::Concede) top = std::max(top, s[i]);
        double sum = 0.0;
        for (size_t i = 0; i < legal.size(); ++i) {
            if (legal[i].type == IntentType::Concede) continue;
            p[i] = std::exp((s[i] - top) / config_.tree_temperature);
            sum += p[i];
        }
        for (double& x : p) x /= sum;
        return p;
    }
    // A kind the policy does not cover: half on ScriptedAgent's pick, the
    // rest shared evenly.
    int options = 0;
    for (const Intent& a : legal)
        if (a.type != IntentType::Concede) ++options;
    const bool has_pick = scripted >= 0 && legal[scripted].type != IntentType::Concede;
    for (size_t i = 0; i < legal.size(); ++i) {
        if (legal[i].type == IntentType::Concede) continue;
        if (!has_pick || options == 1) p[i] = 1.0 / options;
        else p[i] = static_cast<int>(i) == scripted ? 0.5 : 0.5 / (options - 1);
    }
    return p;
}

double HeuristicAgent::leafValue(const GameState& end, PlayerId me) const {
    if (end.game_over) return end.winner == me ? 1.0 : end.winner == opponent(me) ? 0.0 : 0.5;
    if (config_.model) return config_.model->winProbability(end, me, card_db_);
    return 0.5 + 0.5 * std::tanh(evaluate(end, me, config_.weights) / 150.0);
}

Intent HeuristicAgent::treeSearch(const GameState& state, const std::vector<Intent>& legal,
                                  PlayerId me) {
    ++stats_.tree_searches;
    if (!tree_) tree_ = std::make_unique<Tree>();
    Tree& tree = *tree_;
    const int last_turn = lastTurn(state);
    // Nodes from this turn's earlier searches are kept: their simulations
    // played to the same horizon.
    if (tree.searcher != me || tree.turn != state.turn.turn_number || tree.last_turn != last_turn)
        tree.reset(me, state.turn.turn_number, last_turn);

    const uint64_t root_key = treeKey(state, legal, me);
    const std::vector<double> priors = treePriors(state, legal);
    stats_.root_max_prior += *std::max_element(priors.begin(), priors.end());

    int options = 0;
    for (const Intent& a : legal)
        if (a.type != IntentType::Concede) ++options;
    const int sims = std::min(config_.tree_sims, config_.tree_sims_per_action * options);
    bool budget_spent = false;
    if (config_.tree_halving) {
        const int pick = treeHalving(state, legal, me, root_key, priors, sims, budget_spent);
        if (pick < 0) {
            if (budget_spent) ++stats_.budget_fallbacks;
            return defaultAction(state, legal);
        }
        if (legal[pick].type == IntentType::EndTurn) {
            ++stats_.lookahead_end_turns;
            countUnspentEnergy(state, legal, me);
        }
        return legal[pick];
    }
    for (int s = 0; s < sims; ++s) {
        if (config_.max_rollout_steps_per_turn > 0 &&
            steps_this_turn_ >= config_.max_rollout_steps_per_turn) {
            budget_spent = true;
            break;
        }
        treeSimulation(state, me, root_key, legal, priors);
    }
    stats_.max_tree_nodes = std::max(stats_.max_tree_nodes, static_cast<int>(tree.nodes.size()));

    // The most visited option; ties go to the better mean.
    const auto it = tree.nodes.find(root_key);
    int best = -1;
    const Tree::Child* best_child = nullptr;
    std::vector<double> scores(legal.size(), std::nan(""));
    last_root_.visits.assign(legal.size(), 0);
    last_root_.priors = priors;
    if (it != tree.nodes.end()) {
        for (size_t j = 0; j < legal.size(); ++j) {
            const int k = it->second.find(legal[j]);
            if (k < 0) continue;
            const Tree::Child& c = it->second.children[k];
            last_root_.visits[j] = c.visits;
            if (c.visits == 0) continue;
            scores[j] = c.mean();
            if (!best_child || c.visits > best_child->visits ||
                (c.visits == best_child->visits && c.mean() > best_child->mean())) {
                best_child = &c;
                best = static_cast<int>(j);
            }
        }
    }
    if (best < 0) {
        if (budget_spent) ++stats_.budget_fallbacks;
        return defaultAction(state, legal);
    }
    if (observer_) observer_(state, legal, scores);
    if (legal[best].type == IntentType::EndTurn) {
        ++stats_.lookahead_end_turns;
        countUnspentEnergy(state, legal, me);
    }
    return legal[best];
}

int HeuristicAgent::treeHalving(const GameState& state, const std::vector<Intent>& legal,
                                PlayerId me, uint64_t root_key, const std::vector<double>& priors,
                                int sims, bool& budget_spent) {
    Tree& tree = *tree_;
    std::vector<int> alive;
    for (size_t i = 0; i < legal.size(); ++i)
        if (legal[i].type != IntentType::Concede) alive.push_back(static_cast<int>(i));
    std::stable_sort(alive.begin(), alive.end(),
                     [&](int a, int b) { return priors[a] > priors[b]; });
    if (config_.tree_halving_m > 0 && alive.size() > static_cast<size_t>(config_.tree_halving_m))
        alive.resize(config_.tree_halving_m);
    if (alive.size() <= 1) return alive.empty() ? -1 : alive[0];

    auto child = [&](int i) -> const Tree::Child* {
        const auto it = tree.nodes.find(root_key);
        if (it == tree.nodes.end()) return nullptr;
        const int k = it->second.find(legal[i]);
        return k < 0 || it->second.children[k].visits == 0 ? nullptr : &it->second.children[k];
    };
    // Gumbel MuZero's score without the Gumbel noise: log prior +
    // sigma(normalized mean), sigma growing with the most visits.
    auto rank = [&]() {
        int max_visits = 0;
        for (int i : alive)
            if (const auto* c = child(i)) max_visits = std::max(max_visits, c->visits);
        const double sigma = (50.0 + max_visits) * config_.tree_halving_scale;
        std::vector<double> score(legal.size(), -std::numeric_limits<double>::infinity());
        for (int i : alive) {
            const auto* c = child(i);
            if (!c) continue;  // never simulated (every attempt failed): last
            score[i] = std::log(std::max(priors[i], 1e-9)) + sigma * tree.normalized(c->mean(), true);
        }
        std::stable_sort(alive.begin(), alive.end(),
                         [&](int a, int b) { return score[a] > score[b]; });
    };

    const int rounds = std::max(1, static_cast<int>(std::ceil(std::log2(alive.size()))));
    for (int r = 0; r < rounds && alive.size() > 1 && !budget_spent; ++r) {
        const int per = std::max(1, sims / (rounds * static_cast<int>(alive.size())));
        for (int v = 0; v < per && !budget_spent; ++v) {
            const uint64_t seed = rng_() | 1;
            for (int i : alive) {
                if (config_.max_rollout_steps_per_turn > 0 &&
                    steps_this_turn_ >= config_.max_rollout_steps_per_turn) {
                    budget_spent = true;
                    break;
                }
                treeSimulation(state, me, root_key, legal, priors, i, seed);
            }
        }
        rank();
        alive.resize((alive.size() + 1) / 2);
    }
    if (alive.size() > 1) rank();

    std::vector<double> scores(legal.size(), std::nan(""));
    last_root_.visits.assign(legal.size(), 0);
    last_root_.priors = priors;
    for (size_t j = 0; j < legal.size(); ++j)
        if (const auto* c = child(static_cast<int>(j))) {
            scores[j] = c->mean();
            last_root_.visits[j] = c->visits;
        }
    if (!child(alive[0])) return -1;  // nothing was simulated
    if (observer_) observer_(state, legal, scores);
    return alive[0];
}

bool HeuristicAgent::treeSimulation(const GameState& state, PlayerId me, uint64_t root_key,
                                    const std::vector<Intent>& root_legal,
                                    const std::vector<double>& root_priors,
                                    int forced, uint64_t seed) {
    Tree& tree = *tree_;
    struct Visit {
        Tree::Node* node;
        int child;
        bool mine;
    };
    std::vector<Visit> path;
    int steps = 0;
    double value = 0.5;
    try {
        GameState guess = state;
        // A given seed: the same guess and engine seed for every option
        // compared with it (sequential halving's common random numbers).
        std::mt19937_64 shared(seed);
        std::mt19937_64& rng = seed ? shared : rng_;
        determinize(guess, me, rng);
        EventBus bus;
        GameEngine engine(card_db_, bus, card_registry_);
        // Nonzero: resumeFromSnapshot treats 0 as "seed from random_device".
        StepResult step = engine.resumeFromSnapshot(guess, rng() | 1);
        if (step.kind != StepKind::NeedDecision) {
            ++stats_.failed_simulations;
            return false;
        }
        bool in_tree = true, at_root = true;
        while (step.kind == StepKind::NeedDecision &&
               engine.state().turn.turn_number <= tree.last_turn &&
               steps < config_.max_rollout_steps) {
            const std::vector<Intent>& legal = step.legal;
            int pick = -1;
            const bool rune = legal.size() >= 2 && in_tree &&
                std::strcmp(ml::policyDecisionKind(engine.state(), legal), "rune") == 0;
            const long tree_steps_before = stats_.tree_steps;
            // A rune payment gets no node with tree_skip_rune: it is answered
            // below as in a playout, and the tree goes on after it.
            if (legal.size() >= 2 && in_tree && !(rune && config_.tree_skip_rune && !at_root)) {
                const uint64_t key = at_root ? root_key : treeKey(engine.state(), legal, me);
                auto [it, created] = tree.nodes.try_emplace(key);
                if (created && !at_root) {
                    in_tree = false;  // a new leaf: the playout starts here
                } else if (at_root && forced >= 0) {
                    pick = findAction(legal, root_legal[forced]);
                    if (pick < 0) {  // the guess does not offer it
                        ++stats_.failed_simulations;
                        return false;
                    }
                    const int child = tree.force(it->second, legal[pick], root_priors[forced]);
                    path.push_back({&it->second, child, legal[0].player == me});
                    ++stats_.tree_steps;
                } else {
                    const bool mine = legal[0].player == me;
                    const std::vector<double> priors =
                        at_root && legal == root_legal ? root_priors : treePriors(engine.state(), legal);
                    int child = -1;
                    pick = tree.select(it->second, legal, priors,
                                       at_root ? 0 : static_cast<size_t>(config_.tree_width),
                                       config_.tree_c, mine, child, config_.tree_fpu);
                    if (pick >= 0) {
                        path.push_back({&it->second, child, mine});
                        ++stats_.tree_steps;
                    } else {
                        in_tree = false;
                    }
                }
                at_root = false;
            }
            if (rune && stats_.tree_steps > tree_steps_before) ++stats_.tree_rune_steps;
            if (pick < 0) pick = std::max(0, defaultIndex(engine.state(), legal, config_.policy_rollouts));
            step = engine.applyChoice(pick);
            ++steps;
        }
        value = leafValue(engine.state(), me);
    } catch (const std::exception&) {
        for (const Visit& v : path) {
            --v.node->visits;
            --v.node->children[v.child].visits;
        }
        ++stats_.failed_simulations;
        return false;
    }
    stats_.rollout_steps += steps;
    steps_this_turn_ += steps;
    ++stats_.simulations;
    for (const Visit& v : path) {
        Tree::Child& c = v.node->children[v.child];
        c.value_sum += v.mine ? value : 1.0 - value;
        tree.observe(c.mean(), v.mine);
    }
    return true;
}

std::optional<Intent> HeuristicAgent::treeAnswer(const GameState& state,
                                                 const std::vector<Intent>& legal) {
    if (!tree_ || legal.size() < 2) return std::nullopt;
    const PlayerId me = legal[0].player;
    const Tree& tree = *tree_;
    if (tree.searcher != me || tree.turn != state.turn.turn_number) return std::nullopt;
    const auto it = tree.nodes.find(treeKey(state, legal, me));
    if (it == tree.nodes.end() || it->second.visits < config_.tree_min_reuse) return std::nullopt;

    int best = -1, known = 0;
    const Tree::Child* best_child = nullptr;
    std::vector<double> scores(legal.size(), std::nan(""));
    last_root_.visits.assign(legal.size(), 0);
    last_root_.priors.assign(legal.size(), std::nan(""));
    for (size_t j = 0; j < legal.size(); ++j) {
        const int k = it->second.find(legal[j]);
        if (k < 0) continue;
        const Tree::Child& c = it->second.children[k];
        last_root_.visits[j] = c.visits;
        last_root_.priors[j] = c.prior;
        if (c.visits == 0) continue;
        scores[j] = c.mean();
        ++known;
        if (!best_child || c.visits > best_child->visits ||
            (c.visits == best_child->visits && c.mean() > best_child->mean())) {
            best_child = &c;
            best = static_cast<int>(j);
        }
    }
    if (best < 0) return std::nullopt;
    if (observer_ && known >= 2) observer_(state, legal, scores);
    ++stats_.tree_answers;
    return legal[best];
}

} // namespace riftbound
