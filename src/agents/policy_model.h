#pragma once
/// @file policy_model.h
/// Learned policy: gradient-boosted ranking trees that score each legal
/// action of a decision (ml::extractPolicyFeatures), trained by
/// riftbound-trainer's policy_gbt.py to rank actions as the heuristic
/// agent's search did. Read from the JSON it exports:
///
///   { "riftbound_policy_feature_version": N,  // must match ml::kPolicyFeatureVersion
///     "kinds": ["main", "showdown", ...],    // decision kinds it was trained on
///     "lightgbm": <LightGBM Booster.dump_model()> }
///
/// HeuristicAgent uses it in place of ScriptedAgent — in its simulations,
/// and for the decisions it does not search — at the decision kinds the
/// model covers.

#include "gbt_evaluator.h"

#include "core/card_db.h"
#include "core/game_state.h"
#include "core/intent.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace riftbound {

class PolicyModel {
public:
    /// Load an exported model; throws std::runtime_error on a bad file or a
    /// feature-layout version mismatch.
    static std::shared_ptr<const PolicyModel> load(const std::string& path);

    /// Whether the model was trained on decisions of this kind
    /// (ml::policyDecisionKind).
    bool covers(const char* kind) const { return kinds_.count(kind) > 0; }

    /// Score of every legal action (higher is better). `scripted_index` is
    /// ScriptedAgent's pick, a feature of the encoding.
    std::vector<double> scores(const GameState& state, const CardDB& card_db,
                               const std::vector<Intent>& legal, const char* kind,
                               int scripted_index) const;

    /// Index of the best-scoring action (ties: the earlier one), or -1 when
    /// the model does not cover this kind of decision.
    int pick(const GameState& state, const CardDB& card_db, const std::vector<Intent>& legal,
             int scripted_index) const;

    int numTrees() const { return trees_.numTrees(); }

private:
    GbtTrees trees_;
    std::set<std::string, std::less<>> kinds_;
};

} // namespace riftbound
