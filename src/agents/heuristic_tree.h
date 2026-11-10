#pragma once
/// @file heuristic_tree.h
/// The search tree of HeuristicAgent's tree search (HeuristicConfig::tree),
/// shared by heuristic_agent.cpp and heuristic_tree.cpp. Not a public header.

#include "heuristic_agent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace riftbound {

struct HeuristicAgent::Tree {
    struct Child {
        Intent action;
        double prior = 0.0;      // the policy's, averaged over the visits it was available in
        int available = 0;       // visits of the node in which the option was legal
        int visits = 0;
        double value_sum = 0.0;  // for the node's player, in [0, 1]
        double mean() const { return value_sum / visits; }
    };
    struct Node {
        std::vector<Child> children;
        int visits = 0;
        int find(const Intent& action) const {
            for (size_t k = 0; k < children.size(); ++k)
                if (children[k].action == action) return static_cast<int>(k);
            return -1;
        }
    };

    // Keyed by HeuristicAgent::treeKey. Nodes are referenced across
    // insertions, which unordered_map allows.
    std::unordered_map<uint64_t, Node> nodes;
    PlayerId searcher = PlayerId::None;
    int turn = -1;       // turn_number of the searches the nodes come from
    int last_turn = -1;  // the turn their simulations play to
    // Range of the children's means so far, from the searcher's side.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();

    void reset(PlayerId me, int turn_number, int last) {
        nodes.clear();
        searcher = me;
        turn = turn_number;
        last_turn = last;
        lo = std::numeric_limits<double>::infinity();
        hi = -std::numeric_limits<double>::infinity();
    }

    /// A mean for the node's player mapped to [0, 1] by the range so far.
    double normalized(double mean, bool mine) const {
        if (!(hi > lo)) return 0.5;
        const double v = mine ? mean : 1.0 - mean;
        const double x = (v - lo) / (hi - lo);
        return mine ? x : 1.0 - x;
    }
    void observe(double mean, bool mine) {
        const double v = mine ? mean : 1.0 - mean;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }

    /// Takes `action` at `node` (sequential halving's root), counting the
    /// visit; returns its index in node.children.
    int force(Node& node, const Intent& action, double prior) {
        int k = node.find(action);
        if (k < 0) {
            node.children.push_back(Child{action});
            k = static_cast<int>(node.children.size()) - 1;
        }
        Child& ch = node.children[k];
        ++ch.available;
        ch.prior += (prior - ch.prior) / ch.available;
        ++node.visits;
        ++ch.visits;
        return k;
    }

    /// PUCT: the index in `legal` of the option to take at `node`, counting
    /// the visit; `child` gets its index in node.children. Candidates are
    /// the `width` options with the best priors (0: all). Concede never is.
    /// `fpu` >= 0: an unvisited option is valued at the node's mean so far
    /// minus `fpu` (HeuristicConfig::tree_fpu) instead of the worst seen.
    int select(Node& node, const std::vector<Intent>& legal, const std::vector<double>& priors,
               size_t width, double c, bool mine, int& child, double fpu = -1.0) {
        std::vector<int> order;
        order.reserve(legal.size());
        for (size_t i = 0; i < legal.size(); ++i)
            if (legal[i].type != IntentType::Concede) order.push_back(static_cast<int>(i));
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return priors[a] > priors[b]; });
        if (width > 0 && order.size() > width) order.resize(width);

        double unvisited_q = 0.0;
        if (fpu >= 0.0) {
            double sum = 0.0;
            int visits = 0;
            for (const Child& ch : node.children) {
                sum += ch.value_sum;
                visits += ch.visits;
            }
            if (visits > 0) unvisited_q = std::max(0.0, normalized(sum / visits, mine) - fpu);
        }

        int best = -1;
        double best_score = -std::numeric_limits<double>::infinity();
        for (int i : order) {
            int k = node.find(legal[i]);
            if (k < 0) {
                node.children.push_back(Child{legal[i]});
                k = static_cast<int>(node.children.size()) - 1;
            }
            Child& ch = node.children[k];
            ++ch.available;
            ch.prior += (priors[i] - ch.prior) / ch.available;
            // Unvisited options count as the worst seen (MuZero); the prior
            // decides which of them is tried first.
            const double q = ch.visits > 0 ? normalized(ch.mean(), mine) : unvisited_q;
            const double score = q + c * ch.prior * std::sqrt(static_cast<double>(ch.available)) /
                                         (1.0 + ch.visits);
            if (score > best_score) {
                best_score = score;
                best = i;
                child = k;
            }
        }
        if (best >= 0) {
            ++node.visits;
            ++node.children[child].visits;
        }
        return best;
    }
};

} // namespace riftbound
