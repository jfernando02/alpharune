#include "policy_model.h"

#include "nn_model.h"

#include "ml/obs_encoder.h"
#include "ml/policy_features.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace riftbound {

using json = nlohmann::json;

std::shared_ptr<const PolicyModel> PolicyModel::load(const std::string& path) {
    if (NnModel::isNnFile(path)) {
        auto model = std::shared_ptr<PolicyModel>(new PolicyModel());
        model->nn_ = NnModel::load(path);
        if (!model->nn_->hasPolicy())
            throw std::runtime_error("PolicyModel: " + path + " has no policy head");
        model->kinds_ = model->nn_->kinds();
        return model;
    }
    std::ifstream in(path);
    if (!in) throw std::runtime_error("PolicyModel: cannot open " + path);
    const json root = json::parse(in);

    const int version = root.value("riftbound_policy_feature_version", -1);
    if (version != ml::kPolicyFeatureVersion)
        throw std::runtime_error("PolicyModel: " + path + " has feature version " +
                                 std::to_string(version) + ", the engine encodes version " +
                                 std::to_string(ml::kPolicyFeatureVersion) + "; retrain it");

    auto model = std::shared_ptr<PolicyModel>(new PolicyModel());
    for (const auto& k : root.at("kinds")) model->kinds_.insert(k.get<std::string>());
    model->trees_.load(&root.at("lightgbm"), ml::kPolicyFeatureDim, path);
    return model;
}

std::vector<double> PolicyModel::scores(const GameState& state, const CardDB& card_db,
                                        const std::vector<Intent>& legal, const char* kind,
                                        int scripted_index) const {
    thread_local ml::SparseRow position, row;
    std::vector<double> out(legal.size(), 0.0);
    if (legal.empty()) return out;
    if (nn_) {
        thread_local ml::Obs obs;
        thread_local std::vector<ml::ObsAction> actions;
        ml::encodeObs(state, legal[0].player, card_db, obs);
        ml::encodeActions(state, card_db, legal, obs, kind, scripted_index, actions);
        nn_->forward(obs, &actions, nullptr, &out);
        return out;
    }
    ml::extractValueFeatures(state, legal[0].player, position);
    for (size_t i = 0; i < legal.size(); ++i) {
        ml::extractPolicyFeatures(state, card_db, legal, static_cast<int>(i), position, kind,
                                  scripted_index, row);
        out[i] = trees_.rawScore(row);
    }
    return out;
}

int PolicyModel::pick(const GameState& state, const CardDB& card_db,
                      const std::vector<Intent>& legal, int scripted_index) const {
    if (legal.empty()) return -1;
    const char* kind = ml::policyDecisionKind(state, legal);
    if (!covers(kind)) return -1;
    const std::vector<double> s = scores(state, card_db, legal, kind, scripted_index);
    int best = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (legal[i].type == IntentType::Concede) continue;
        if (best < 0 || s[i] > s[best]) best = static_cast<int>(i);
    }
    return best;
}

} // namespace riftbound
