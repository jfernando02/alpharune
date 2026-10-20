#pragma once
/// @file gbt_evaluator.h
/// Gradient-boosted-tree value model, read from the JSON that
/// riftbound-trainer's value_gbt.py exports:
///
///   { "riftbound_value_feature_version": N,   // must match ml::kValueFeatureVersion
///     "lightgbm": <LightGBM Booster.dump_model()> }
///
/// It is evaluated here without LightGBM: each tree is flattened to an array
/// of nodes; a row goes left when its value is <= the split threshold
/// (missing-value rules as LightGBM's: "Zero" sends zeros the default way,
/// "NaN" sends NaN). The raw score is the sum of the leaves, the win
/// probability sigmoid(raw * sigmoid_coefficient).

#include "evaluator.h"

#include "ml/value_features.h"

#include <memory>
#include <string>
#include <vector>

namespace riftbound {

class GbtModel : public PositionModel {
public:
    /// Load an exported model; throws std::runtime_error on a bad file or a
    /// feature-layout version mismatch.
    static std::shared_ptr<const GbtModel> load(const std::string& path);

    /// Sum of the trees' leaves for one encoded position.
    double rawScore(const ml::SparseRow& row) const;

    double winProbability(const GameState& state, PlayerId me) const override;

    int numTrees() const { return static_cast<int>(tree_roots_.size()); }

private:
    struct Node {
        int feature = -1;            // -1: leaf
        double threshold = 0.0;
        int left = -1, right = -1;   // indices into nodes_
        bool default_left = true;
        enum class Missing : uint8_t { None, Zero, NaN } missing = Missing::None;
        double leaf_value = 0.0;
    };

    int addNode(const void* json_node);  // nlohmann::json*, kept out of the header

    std::vector<Node> nodes_;
    std::vector<int> tree_roots_;
    double sigmoid_ = 1.0;
};

} // namespace riftbound
