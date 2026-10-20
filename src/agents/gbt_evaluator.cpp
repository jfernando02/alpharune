#include "gbt_evaluator.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>

namespace riftbound {

using json = nlohmann::json;

std::shared_ptr<const GbtModel> GbtModel::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("GbtModel: cannot open " + path);
    const json root = json::parse(in);

    const int version = root.value("riftbound_value_feature_version", -1);
    if (version != ml::kValueFeatureVersion)
        throw std::runtime_error("GbtModel: " + path + " has feature version " +
                                 std::to_string(version) + ", the engine encodes version " +
                                 std::to_string(ml::kValueFeatureVersion) + "; retrain it");

    const json& lgb = root.at("lightgbm");
    if (lgb.value("num_class", 1) != 1 || lgb.value("num_tree_per_iteration", 1) != 1)
        throw std::runtime_error("GbtModel: only binary models are supported");
    if (lgb.value("max_feature_idx", 0) >= ml::kValueFeatureDim)
        throw std::runtime_error("GbtModel: model uses more features than the encoding has");

    auto model = std::shared_ptr<GbtModel>(new GbtModel());
    // "binary sigmoid:1"
    const std::string objective = lgb.value("objective", "binary sigmoid:1");
    const auto colon = objective.find("sigmoid:");
    if (objective.rfind("binary", 0) != 0 || colon == std::string::npos)
        throw std::runtime_error("GbtModel: unsupported objective '" + objective + "'");
    model->sigmoid_ = std::stod(objective.substr(colon + 8));

    for (const json& tree : lgb.at("tree_info"))
        model->tree_roots_.push_back(model->addNode(&tree.at("tree_structure")));
    return model;
}

int GbtModel::addNode(const void* json_node) {
    const json& j = *static_cast<const json*>(json_node);
    const int index = static_cast<int>(nodes_.size());
    nodes_.emplace_back();
    if (j.contains("leaf_value")) {
        nodes_[index].leaf_value = j.at("leaf_value").get<double>();
        return index;
    }
    if (j.value("decision_type", "<=") != "<=")
        throw std::runtime_error("GbtModel: categorical splits are not supported");

    Node node;
    node.feature = j.at("split_feature").get<int>();
    node.threshold = j.at("threshold").get<double>();
    node.default_left = j.value("default_left", true);
    const std::string missing = j.value("missing_type", "None");
    node.missing = missing == "Zero" ? Node::Missing::Zero
                 : missing == "NaN"  ? Node::Missing::NaN
                                     : Node::Missing::None;
    node.left = addNode(&j.at("left_child"));
    node.right = addNode(&j.at("right_child"));
    nodes_[index] = node;  // after the children: emplace_back may have moved nodes_
    return index;
}

double GbtModel::rawScore(const ml::SparseRow& row) const {
    // Dense copy of the row; reused per thread, cleared after use.
    thread_local std::vector<double> x(ml::kValueFeatureDim, 0.0);
    for (const auto& [i, v] : row) x[i] = v;

    double sum = 0.0;
    for (int n : tree_roots_) {
        while (nodes_[n].feature >= 0) {
            const Node& node = nodes_[n];
            const double v = x[node.feature];
            bool go_left;
            if (node.missing == Node::Missing::Zero && v == 0.0) go_left = node.default_left;
            else if (node.missing == Node::Missing::NaN && std::isnan(v)) go_left = node.default_left;
            else go_left = v <= node.threshold;
            n = go_left ? node.left : node.right;
        }
        sum += nodes_[n].leaf_value;
    }

    for (const auto& [i, v] : row) x[i] = 0.0;
    return sum;
}

double GbtModel::winProbability(const GameState& state, PlayerId me) const {
    thread_local ml::SparseRow row;
    ml::extractValueFeatures(state, me, row);
    return 1.0 / (1.0 + std::exp(-sigmoid_ * rawScore(row)));
}

} // namespace riftbound
