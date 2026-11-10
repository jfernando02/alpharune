#pragma once
/// @file nn_model.h
/// Neural policy / value model over the structured observation
/// (ml/obs_encoder), trained by riftbound-trainer's nn_train.py and run here
/// without any ML library: the engine has no torch dependency.
///
/// The network (mirrored line for line by nn_model.py in the trainer):
///
///   x_i   = ent_in(f_i * ent_scale) + card_emb[card_i] + group_emb[group_i]
///   h_i   = relu(ent_l2(relu(x_i + att(sum of relu(x_j) over gear j attached to i))))
///   pools = sum and max of h_i per pool (bases, hand, facedown, chain, and
///           each side's units at each battlefield)
///   b_k   = relu(bf([h of battlefield k, its two pools' sum and max]))
///   t     = [globals * glob_scale, the fixed pools, sum and max of b_k,
///            both legends' and champion-zone cards' h, the three bags]
///   s     = relu(trunk2(relu(trunk1(t))))
///   value = val2(relu(val1(s)))                            (a logit)
///   u_a   = pol_s(s) + pol_a(a * act_scale) + pol_act(h[acting])
///           + pol_ch(mean h[chosen]) + pol_mv(mean h[moved]) + pol_dst(b[dest])
///           + pol_ec(card_emb[acting card]) + pol_cc(mean card_emb[chosen cards])
///   logit_a = pol2(relu(pol1(relu(u_a))))
///
/// File: the 8 bytes "RBNN0001", a little-endian uint32 header length, a JSON
/// header ({obs_version, config, kinds, has_value, has_policy, tensors:
/// [{name, shape, offset}]}), then the float32 data the offsets index.
///
/// A loaded model is immutable and safe to share between threads.

#include "evaluator.h"

#include "core/card_db.h"
#include "ml/obs_encoder.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace riftbound {

class NnModel {
public:
    /// True when `path` starts with the model file's magic bytes.
    static bool isNnFile(const std::string& path);
    /// Load a model; throws std::runtime_error on a bad file or one trained
    /// for another observation layout.
    static std::shared_ptr<const NnModel> load(const std::string& path);

    bool hasValue() const { return has_value_; }
    bool hasPolicy() const { return has_policy_; }
    bool covers(const char* kind) const { return kinds_.count(kind) > 0; }
    const std::set<std::string, std::less<>>& kinds() const { return kinds_; }

    /// Runs the network on one observation. `value_logit` (if not null) gets
    /// the value head's logit; `logits` (if not null) one score per action.
    void forward(const ml::Obs& obs, const std::vector<ml::ObsAction>* actions,
                 double* value_logit, std::vector<double>* logits) const;

    /// The player's chance of winning, in [0, 1].
    double winProbability(const ml::Obs& obs) const;

    int entityWidth() const { return d_; }
    int trunkWidth() const { return D_; }

private:
    /// A dense layer stored column by column, so y += W x runs as one
    /// vectorizable y += x_j * column_j per nonzero input.
    struct Linear {
        int in = 0, out = 0;
        std::vector<float> wt;  // [in][out]
        std::vector<float> b;   // [out], zeros when the layer has no bias
        void add(const float* x, float* y) const { addAt(x, in, 0, y); }
        /// y += W[:, offset : offset + n] x
        void addAt(const float* x, int n, int offset, float* y) const;
    };

    int d_ = 0, D_ = 0, db_ = 0, dp_ = 0, dq_ = 0, dv_ = 0;
    bool has_value_ = false, has_policy_ = false;
    std::set<std::string, std::less<>> kinds_;
    std::vector<float> glob_scale_, ent_scale_, act_scale_;
    std::vector<float> card_emb_, bag_emb_, group_emb_;
    Linear ent_in_, att_, ent_l2_, bf_, trunk1_, trunk2_, val1_, val2_;
    Linear pol_s_, pol_a_, pol_act_, pol_ch_, pol_mv_, pol_dst_, pol_ec_, pol_cc_, pol1_, pol2_;
};

/// NnModel as the heuristic agent's position evaluator.
class NnPositionModel : public PositionModel {
public:
    explicit NnPositionModel(std::shared_ptr<const NnModel> model) : model_(std::move(model)) {}
    /// The encoding reads printed costs from the card database.
    double winProbability(const GameState& state, PlayerId me) const override;
    double winProbability(const GameState& state, PlayerId me, const CardDB& card_db) const override;
    const NnModel& model() const { return *model_; }

private:
    std::shared_ptr<const NnModel> model_;
};

/// A value model from either kind of file: the neural model (NnModel) or the
/// gradient-boosted trees' JSON (GbtModel).
std::shared_ptr<const PositionModel> loadPositionModel(const std::string& path);

} // namespace riftbound
