#include "nn_model.h"

#include "gbt_evaluator.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace riftbound {

using json = nlohmann::json;

namespace {

constexpr char kMagic[8] = {'R', 'B', 'N', 'N', '0', '0', '0', '1'};
constexpr float kBagScale = 0.125f;
constexpr int kSingles = 4;  // both legends, both champion-zone cards

// y += a * x. Its own function so the compiler can build an AVX2 version
// and pick it at load time where the CPU has it.
#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__) && defined(__linux__)
__attribute__((target_clones("avx2", "default")))
#endif
void axpy(float a, const float* x, float* y, int n) {
    for (int i = 0; i < n; ++i) y[i] += a * x[i];
}

void relu(float* x, int n) {
    for (int i = 0; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.0f;
}

struct Tensor {
    std::vector<int> shape;
    const float* data = nullptr;
    size_t size = 0;
};

/// Working memory of one forward pass, reused per thread.
struct Scratch {
    std::vector<float> x, h0, h, att, pools_sum, pools_max, b, t, t1, s, v1, ps, u, q;
    std::vector<int> bf_entity;  // battlefield entities, in order
};

} // namespace

void NnModel::Linear::addAt(const float* x, int n, int offset, float* y) const {
    for (int j = 0; j < n; ++j) {
        const float v = x[j];
        if (v != 0.0f) axpy(v, &wt[static_cast<size_t>(offset + j) * out], y, out);
    }
}

bool NnModel::isNnFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    return in.read(magic, 8) && std::memcmp(magic, kMagic, 8) == 0;
}

std::shared_ptr<const NnModel> NnModel::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("NnModel: cannot open " + path);
    char magic[8] = {};
    uint32_t header_len = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 ||
        !in.read(reinterpret_cast<char*>(&header_len), 4))
        throw std::runtime_error("NnModel: " + path + " is not a model file");
    std::string header_text(header_len, '\0');
    if (!in.read(header_text.data(), header_len))
        throw std::runtime_error("NnModel: " + path + " is truncated");
    const json header = json::parse(header_text);
    std::vector<float> data;
    {
        const auto start = in.tellg();
        in.seekg(0, std::ios::end);
        const auto bytes = static_cast<size_t>(in.tellg() - start);
        in.seekg(start);
        data.resize(bytes / sizeof(float));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    }

    const auto fail = [&](const std::string& what) -> std::runtime_error {
        return std::runtime_error("NnModel: " + path + ": " + what);
    };
    if (header.value("obs_version", -1) != ml::kObsVersion)
        throw fail("trained for observation version " +
                   std::to_string(header.value("obs_version", -1)) + ", the engine encodes version " +
                   std::to_string(ml::kObsVersion) + "; retrain it");
    const json& config = header.at("config");
    const auto expect = [&](const char* key, int value) {
        if (config.value(key, -1) != value)
            throw fail(std::string("config.") + key + " is " + std::to_string(config.value(key, -1)) +
                       ", the engine has " + std::to_string(value));
    };
    expect("globals", ml::kObsGlobalDim);
    expect("entity_features", ml::kObsEntityDim);
    expect("action_features", ml::kObsActionDim);
    expect("card_vocab", ml::kObsCardVocab);
    expect("groups", ml::kObsGroupSlots);
    expect("pools", ml::kObsPoolCount);
    expect("max_battlefields", ml::kObsMaxBattlefields);

    std::map<std::string, Tensor> tensors;
    for (const json& t : header.at("tensors")) {
        Tensor out;
        out.shape = t.at("shape").get<std::vector<int>>();
        out.size = 1;
        for (int n : out.shape) out.size *= static_cast<size_t>(n);
        const size_t offset = t.at("offset").get<size_t>();
        if (offset + out.size > data.size()) throw fail("tensor " + t.at("name").get<std::string>() + " is out of range");
        out.data = data.data() + offset;
        tensors[t.at("name").get<std::string>()] = out;
    }
    const auto get = [&](const std::string& name) -> const Tensor& {
        const auto it = tensors.find(name);
        if (it == tensors.end()) throw fail("missing tensor " + name);
        return it->second;
    };
    const auto vec = [&](const std::string& name, size_t size) {
        const Tensor& t = get(name);
        if (t.size != size) throw fail("tensor " + name + " has the wrong size");
        return std::vector<float>(t.data, t.data + t.size);
    };
    const auto linear = [&](const std::string& name, int in_dim, int out_dim, bool bias) {
        const Tensor& w = get(name + ".w");
        if (w.shape != std::vector<int>{out_dim, in_dim}) throw fail("tensor " + name + ".w has the wrong shape");
        Linear l;
        l.in = in_dim;
        l.out = out_dim;
        l.wt.resize(w.size);
        for (int o = 0; o < out_dim; ++o)
            for (int i = 0; i < in_dim; ++i)
                l.wt[static_cast<size_t>(i) * out_dim + o] = w.data[static_cast<size_t>(o) * in_dim + i];
        l.b = bias ? vec(name + ".b", out_dim) : std::vector<float>(out_dim, 0.0f);
        return l;
    };

    auto model = std::shared_ptr<NnModel>(new NnModel());
    NnModel& m = *model;
    m.d_ = config.at("d").get<int>();
    m.D_ = config.at("D").get<int>();
    m.db_ = config.at("db").get<int>();
    m.dp_ = config.at("dp").get<int>();
    m.dq_ = config.at("dq").get<int>();
    m.dv_ = config.at("dv").get<int>();
    m.has_value_ = header.value("has_value", true);
    m.has_policy_ = header.value("has_policy", true);
    for (const auto& k : header.value("kinds", json::array())) m.kinds_.insert(k.get<std::string>());

    const int d = m.d_, D = m.D_;
    const int trunk_in = ml::kObsGlobalDim + 2 * d * ml::kObsPoolBattlefield + 2 * d + kSingles * d +
                         static_cast<int>(ml::ObsBag::Count) * m.db_;
    m.glob_scale_ = vec("glob_scale", ml::kObsGlobalDim);
    m.ent_scale_ = vec("ent_scale", ml::kObsEntityDim);
    m.act_scale_ = vec("act_scale", ml::kObsActionDim);
    m.card_emb_ = vec("card_emb", static_cast<size_t>(ml::kObsCardVocab) * d);
    m.bag_emb_ = vec("bag_emb", static_cast<size_t>(ml::kObsCardVocab) * m.db_);
    m.group_emb_ = vec("group_emb", static_cast<size_t>(ml::kObsGroupSlots) * d);
    m.ent_in_ = linear("ent_in", ml::kObsEntityDim, d, true);
    m.att_ = linear("att", d, d, false);
    m.ent_l2_ = linear("ent_l2", d, d, true);
    m.bf_ = linear("bf", 5 * d, d, true);
    m.trunk1_ = linear("trunk1", trunk_in, D, true);
    m.trunk2_ = linear("trunk2", D, D, true);
    m.val1_ = linear("val1", D, m.dv_, true);
    m.val2_ = linear("val2", m.dv_, 1, true);
    m.pol_s_ = linear("pol_s", D, m.dp_, true);
    m.pol_a_ = linear("pol_a", ml::kObsActionDim, m.dp_, false);
    m.pol_act_ = linear("pol_act", d, m.dp_, false);
    m.pol_ch_ = linear("pol_ch", d, m.dp_, false);
    m.pol_mv_ = linear("pol_mv", d, m.dp_, false);
    m.pol_dst_ = linear("pol_dst", d, m.dp_, false);
    m.pol_ec_ = linear("pol_ec", d, m.dp_, false);
    m.pol_cc_ = linear("pol_cc", d, m.dp_, false);
    m.pol1_ = linear("pol1", m.dp_, m.dq_, true);
    m.pol2_ = linear("pol2", m.dq_, 1, true);
    return model;
}

void NnModel::forward(const ml::Obs& obs, const std::vector<ml::ObsAction>* actions,
                      double* value_logit, std::vector<double>* logits) const {
    thread_local Scratch sc;
    const int d = d_, D = D_;
    const int n = static_cast<int>(obs.entities.size());
    const auto row = [d](std::vector<float>& v, int i) { return v.data() + static_cast<size_t>(i) * d; };

    // ── Entities ──
    sc.x.assign(static_cast<size_t>(n) * d, 0.0f);
    sc.h0.resize(static_cast<size_t>(n) * d);
    sc.h.resize(static_cast<size_t>(n) * d);
    for (int i = 0; i < n; ++i) {
        const ml::ObsEntity& e = obs.entities[i];
        float* x = row(sc.x, i);
        const float* emb = &card_emb_[static_cast<size_t>(e.card < ml::kObsCardVocab ? e.card : 0) * d];
        const float* grp = &group_emb_[static_cast<size_t>(e.group < ml::kObsGroupSlots ? e.group : 0) * d];
        for (int k = 0; k < d; ++k) x[k] = ent_in_.b[k] + emb[k] + grp[k];
        for (int k = 0; k < ml::kObsEntityDim; ++k)
            if (e.f[k] != 0) axpy(static_cast<float>(e.f[k]) * ent_scale_[k], &ent_in_.wt[static_cast<size_t>(k) * d], x, d);
    }
    std::copy(sc.x.begin(), sc.x.end(), sc.h0.begin());
    relu(sc.h0.data(), n * d);
    // Gear into the unit that wears it.
    bool any_host = false;
    for (int i = 0; i < n; ++i)
        if (obs.entities[i].host >= 0 && obs.entities[i].host < n) any_host = true;
    if (any_host) {
        sc.att.assign(static_cast<size_t>(n) * d, 0.0f);
        std::vector<char> hosts(n, 0);
        for (int i = 0; i < n; ++i) {
            const int host = obs.entities[i].host;
            if (host < 0 || host >= n) continue;
            hosts[host] = 1;
            const float* g = row(sc.h0, i);
            float* a = row(sc.att, host);
            for (int k = 0; k < d; ++k) a[k] += g[k];
        }
        for (int i = 0; i < n; ++i)
            if (hosts[i]) att_.add(row(sc.att, i), row(sc.x, i));
        relu(sc.x.data(), n * d);
    } else {
        std::copy(sc.h0.begin(), sc.h0.end(), sc.x.begin());
    }
    for (int i = 0; i < n; ++i) {
        float* h = row(sc.h, i);
        std::copy(ent_l2_.b.begin(), ent_l2_.b.end(), h);
        ent_l2_.add(row(sc.x, i), h);
    }
    relu(sc.h.data(), n * d);

    // ── Pools ──
    sc.pools_sum.assign(static_cast<size_t>(ml::kObsPoolCount) * d, 0.0f);
    sc.pools_max.assign(static_cast<size_t>(ml::kObsPoolCount) * d, 0.0f);
    for (int i = 0; i < n; ++i) {
        const int p = obs.entities[i].pool;
        if (p >= ml::kObsPoolCount) continue;
        const float* h = row(sc.h, i);
        float* s = row(sc.pools_sum, p);
        float* mx = row(sc.pools_max, p);
        for (int k = 0; k < d; ++k) {
            s[k] += h[k];
            mx[k] = std::max(mx[k], h[k]);
        }
    }

    // ── Battlefields ──
    sc.bf_entity.clear();
    for (int i = 0; i < n; ++i)
        if (obs.entities[i].group == static_cast<uint8_t>(ml::ObsGroup::Battlefield)) sc.bf_entity.push_back(i);
    const int nb = static_cast<int>(sc.bf_entity.size());
    sc.b.resize(static_cast<size_t>(std::max(nb, 1)) * d);
    int slot_b[ml::kObsMaxBattlefields];
    std::fill(std::begin(slot_b), std::end(slot_b), -1);
    for (int j = 0; j < nb; ++j) {
        const int i = sc.bf_entity[j];
        const int slot = std::clamp(static_cast<int>(obs.entities[i].place) - 3, 0, ml::kObsMaxBattlefields - 1);
        if (slot_b[slot] < 0) slot_b[slot] = j;
        float* b = row(sc.b, j);
        std::copy(bf_.b.begin(), bf_.b.end(), b);
        const int mine = ml::kObsPoolBattlefield + 2 * slot;
        bf_.addAt(row(sc.h, i), d, 0, b);
        bf_.addAt(row(sc.pools_sum, mine), d, d, b);
        bf_.addAt(row(sc.pools_max, mine), d, 2 * d, b);
        bf_.addAt(row(sc.pools_sum, mine + 1), d, 3 * d, b);
        bf_.addAt(row(sc.pools_max, mine + 1), d, 4 * d, b);
        relu(b, d);
    }

    // ── Trunk ──
    const int trunk_in = trunk1_.in;
    sc.t.assign(trunk_in, 0.0f);
    float* t = sc.t.data();
    for (int k = 0; k < ml::kObsGlobalDim; ++k) t[k] = obs.g[k] * glob_scale_[k];
    t += ml::kObsGlobalDim;
    for (int p = 0; p < ml::kObsPoolBattlefield; ++p) {
        std::copy_n(row(sc.pools_sum, p), d, t);
        std::copy_n(row(sc.pools_max, p), d, t + d);
        t += 2 * d;
    }
    for (int j = 0; j < nb; ++j) {
        const float* b = row(sc.b, j);
        for (int k = 0; k < d; ++k) {
            t[k] += b[k];
            t[d + k] = std::max(t[d + k], b[k]);
        }
    }
    t += 2 * d;
    for (int i = 0; i < n; ++i) {
        const int g = static_cast<int>(obs.entities[i].group) - static_cast<int>(ml::ObsGroup::MyLegend);
        if (g >= 0 && g < kSingles) std::copy_n(row(sc.h, i), d, t + static_cast<size_t>(g) * d);
    }
    t += kSingles * d;
    for (const auto& [bag, card] : obs.bags) {
        if (bag >= static_cast<uint8_t>(ml::ObsBag::Count) || card >= ml::kObsCardVocab) continue;
        axpy(kBagScale, &bag_emb_[static_cast<size_t>(card) * db_], t + static_cast<size_t>(bag) * db_, db_);
    }

    sc.t1.assign(trunk1_.b.begin(), trunk1_.b.end());
    trunk1_.add(sc.t.data(), sc.t1.data());
    relu(sc.t1.data(), D);
    sc.s.assign(trunk2_.b.begin(), trunk2_.b.end());
    trunk2_.add(sc.t1.data(), sc.s.data());
    relu(sc.s.data(), D);

    if (value_logit) {
        sc.v1.assign(val1_.b.begin(), val1_.b.end());
        val1_.add(sc.s.data(), sc.v1.data());
        relu(sc.v1.data(), dv_);
        float out = val2_.b[0];
        val2_.add(sc.v1.data(), &out);
        *value_logit = out;
    }
    if (!logits || !actions) return;

    // ── Policy ──
    logits->assign(actions->size(), 0.0);
    sc.ps.assign(pol_s_.b.begin(), pol_s_.b.end());
    pol_s_.add(sc.s.data(), sc.ps.data());
    sc.u.resize(dp_);
    sc.q.resize(dq_);
    std::vector<float>& mean = sc.v1;  // free again; reused as a d-wide buffer
    mean.resize(std::max(d, dv_));
    const auto entity = [&](int i) { return i >= 0 && i < n ? row(sc.h, i) : nullptr; };
    for (size_t ai = 0; ai < actions->size(); ++ai) {
        const ml::ObsAction& a = (*actions)[ai];
        float* u = sc.u.data();
        std::copy(sc.ps.begin(), sc.ps.end(), u);
        for (int k = 0; k < ml::kObsActionDim; ++k)
            if (a.a[k] != 0) axpy(static_cast<float>(a.a[k]) * act_scale_[k], &pol_a_.wt[static_cast<size_t>(k) * dp_], u, dp_);
        if (const float* h = entity(a.acting)) pol_act_.add(h, u);

        const auto addMean = [&](const Linear& layer, const int16_t* refs, int count) {
            int found = 0;
            std::fill_n(mean.data(), d, 0.0f);
            for (int c = 0; c < count; ++c)
                if (const float* h = entity(refs[c])) {
                    for (int k = 0; k < d; ++k) mean[k] += h[k];
                    ++found;
                }
            if (found == 0) return;
            const float inv = 1.0f / static_cast<float>(found);
            for (int k = 0; k < d; ++k) mean[k] *= inv;
            layer.add(mean.data(), u);
        };
        addMean(pol_ch_, a.chosen, std::min<int>(a.n_chosen, ml::kObsMaxChosen));
        addMean(pol_mv_, a.moved, std::min<int>(a.n_moved, ml::kObsMaxMoved));
        if (a.dest >= 0 && a.dest < ml::kObsMaxBattlefields && slot_b[a.dest] >= 0)
            pol_dst_.add(row(sc.b, slot_b[a.dest]), u);
        if (a.acting_card > 0 && a.acting_card < ml::kObsCardVocab)
            pol_ec_.add(&card_emb_[static_cast<size_t>(a.acting_card) * d], u);
        {
            int found = 0;
            std::fill_n(mean.data(), d, 0.0f);
            for (int c = 0; c < std::min<int>(a.n_chosen, ml::kObsMaxChosen); ++c) {
                const int card = a.chosen_card[c];
                if (card <= 0 || card >= ml::kObsCardVocab) continue;
                const float* e = &card_emb_[static_cast<size_t>(card) * d];
                for (int k = 0; k < d; ++k) mean[k] += e[k];
                ++found;
            }
            if (found > 0) {
                const float inv = 1.0f / static_cast<float>(found);
                for (int k = 0; k < d; ++k) mean[k] *= inv;
                pol_cc_.add(mean.data(), u);
            }
        }
        relu(u, dp_);
        std::copy(pol1_.b.begin(), pol1_.b.end(), sc.q.begin());
        pol1_.add(u, sc.q.data());
        relu(sc.q.data(), dq_);
        float out = pol2_.b[0];
        pol2_.add(sc.q.data(), &out);
        (*logits)[ai] = out;
    }
}

double NnModel::winProbability(const ml::Obs& obs) const {
    double logit = 0.0;
    forward(obs, nullptr, &logit, nullptr);
    return 1.0 / (1.0 + std::exp(-logit));
}

double NnPositionModel::winProbability(const GameState&, PlayerId) const {
    throw std::logic_error("NnPositionModel: winProbability needs the card database");
}

double NnPositionModel::winProbability(const GameState& state, PlayerId me, const CardDB& card_db) const {
    thread_local ml::Obs obs;
    ml::encodeObs(state, me, card_db, obs);
    return model_->winProbability(obs);
}

std::shared_ptr<const PositionModel> loadPositionModel(const std::string& path) {
    if (NnModel::isNnFile(path)) {
        auto model = NnModel::load(path);
        if (!model->hasValue()) throw std::runtime_error("NnModel: " + path + " has no value head");
        return std::make_shared<NnPositionModel>(std::move(model));
    }
    return GbtModel::load(path);
}

} // namespace riftbound
