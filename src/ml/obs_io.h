#pragma once
/// @file obs_io.h
/// Training records for the neural model, as tools/value_data.cpp writes
/// them (--nn-out PREFIX) and riftbound-trainer's nn_data.py reads them.
///
/// A shard is four flat tables of fixed-size little-endian records plus a
/// JSON description:
///
///   PREFIX.pos    one PosRecord per recorded position: whose view, the game's
///                 result for that player, the globals, and where its rows
///                 start in the other tables
///   PREFIX.ent    EntRecord: the position's entities (ml::ObsEntity)
///   PREFIX.act    ActRecord: its legal actions with the search's score of
///                 each (NaN = not searched); none for a value-only position
///   PREFIX.bag    BagRecord: its bag cards
///   PREFIX.meta.json   versions, record sizes, feature names, deck names
///
/// numpy reads each table with one structured dtype; the sizes in the meta
/// file guard against the two sides drifting apart.

#include "ml/obs_encoder.h"

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace riftbound::ml {

#pragma pack(push, 1)
struct PosRecord {
    uint32_t game = 0;
    uint16_t turn = 0;
    uint8_t player = 0;       // 1 or 2
    uint8_t kind = 0;         // obsKindIndex; meaningful when act_count > 0
    int8_t outcome = -1;      // 1 the player won, 0 lost
    uint8_t reserved = 0;
    int16_t chosen = -1;      // index of the search's pick among the actions
    uint16_t my_deck = 0;
    uint16_t opp_deck = 0;
    uint16_t ent_count = 0;
    uint16_t act_count = 0;
    uint16_t bag_count = 0;
    float search_value = 0;   // the search's estimate for the player; NaN if none
    uint64_t ent_start = 0;
    uint64_t act_start = 0;
    uint64_t bag_start = 0;
    float g[kObsGlobalDim] = {};
};
struct EntRecord {
    uint16_t card = 0;
    uint8_t group = 0;
    uint8_t place = 0;
    uint8_t pool = kObsNoPool;
    uint8_t reserved = 0;
    int16_t host = -1;
    int8_t f[kObsEntityDim] = {};
};
struct ActRecord {
    float score = 0;
    int16_t a[kObsActionDim] = {};
    int16_t acting = -1;
    uint16_t acting_card = 0;
    int16_t dest = -1;
    int16_t chosen[kObsMaxChosen] = {};
    uint16_t chosen_card[kObsMaxChosen] = {};
    int16_t moved[kObsMaxMoved] = {};
    uint8_t n_chosen = 0;
    uint8_t n_moved = 0;
};
struct BagRecord {
    uint16_t card = 0;
    uint8_t bag = 0;
    uint8_t reserved = 0;
};
#pragma pack(pop)

inline EntRecord toRecord(const ObsEntity& e) {
    EntRecord r;
    r.card = e.card; r.group = e.group; r.place = e.place; r.pool = e.pool; r.host = e.host;
    for (int i = 0; i < kObsEntityDim; ++i) r.f[i] = e.f[i];
    return r;
}
inline ActRecord toRecord(const ObsAction& a, double score) {
    ActRecord r;
    r.score = static_cast<float>(score);
    for (int i = 0; i < kObsActionDim; ++i) r.a[i] = a.a[i];
    r.acting = a.acting; r.acting_card = a.acting_card; r.dest = a.dest;
    for (int i = 0; i < kObsMaxChosen; ++i) { r.chosen[i] = a.chosen[i]; r.chosen_card[i] = a.chosen_card[i]; }
    for (int i = 0; i < kObsMaxMoved; ++i) r.moved[i] = a.moved[i];
    r.n_chosen = a.n_chosen; r.n_moved = a.n_moved;
    return r;
}
inline ObsEntity fromRecord(const EntRecord& r) {
    ObsEntity e;
    e.card = r.card; e.group = r.group; e.place = r.place; e.pool = r.pool; e.host = r.host;
    for (int i = 0; i < kObsEntityDim; ++i) e.f[i] = r.f[i];
    return e;
}
inline ObsAction fromRecord(const ActRecord& r) {
    ObsAction a;
    for (int i = 0; i < kObsActionDim; ++i) a.a[i] = r.a[i];
    a.acting = r.acting; a.acting_card = r.acting_card; a.dest = r.dest;
    for (int i = 0; i < kObsMaxChosen; ++i) { a.chosen[i] = r.chosen[i]; a.chosen_card[i] = r.chosen_card[i]; }
    for (int i = 0; i < kObsMaxMoved; ++i) a.moved[i] = r.moved[i];
    a.n_chosen = r.n_chosen; a.n_moved = r.n_moved;
    return a;
}

/// One recorded position before its game's result is known.
struct ObsSample {
    PlayerId player = PlayerId::None;
    int turn = 0;
    int kind = 0;
    int chosen = -1;
    double search_value = std::nan("");
    Obs obs;
    std::vector<ObsAction> actions;
    std::vector<double> scores;
};

/// Appends positions to a shard. Not thread-safe: the caller serializes
/// calls to write (one game at a time).
class ObsShardWriter {
public:
    explicit ObsShardWriter(const std::string& prefix)
        : prefix_(prefix),
          pos_(prefix + ".pos", std::ios::binary), ent_(prefix + ".ent", std::ios::binary),
          act_(prefix + ".act", std::ios::binary), bag_(prefix + ".bag", std::ios::binary) {
        if (!pos_ || !ent_ || !act_ || !bag_) throw std::runtime_error("cannot write " + prefix + ".*");
    }

    void write(const ObsSample& s, uint32_t game, int outcome, int my_deck, int opp_deck) {
        PosRecord p;
        p.game = game;
        p.turn = static_cast<uint16_t>(s.turn);
        p.player = static_cast<uint8_t>(s.player);
        p.kind = static_cast<uint8_t>(s.kind);
        p.outcome = static_cast<int8_t>(outcome);
        p.chosen = static_cast<int16_t>(s.chosen);
        p.my_deck = static_cast<uint16_t>(my_deck);
        p.opp_deck = static_cast<uint16_t>(opp_deck);
        p.ent_count = static_cast<uint16_t>(s.obs.entities.size());
        p.act_count = static_cast<uint16_t>(s.actions.size());
        p.bag_count = static_cast<uint16_t>(s.obs.bags.size());
        p.search_value = static_cast<float>(s.search_value);
        p.ent_start = entities_;
        p.act_start = actions_;
        p.bag_start = bags_;
        for (int i = 0; i < kObsGlobalDim; ++i) p.g[i] = s.obs.g[i];
        pos_.write(reinterpret_cast<const char*>(&p), sizeof p);
        for (const ObsEntity& e : s.obs.entities) {
            const EntRecord r = toRecord(e);
            ent_.write(reinterpret_cast<const char*>(&r), sizeof r);
        }
        for (size_t i = 0; i < s.actions.size(); ++i) {
            const ActRecord r = toRecord(s.actions[i], s.scores[i]);
            act_.write(reinterpret_cast<const char*>(&r), sizeof r);
        }
        for (const auto& [bag, card] : s.obs.bags) {
            BagRecord r;
            r.bag = bag;
            r.card = card;
            bag_.write(reinterpret_cast<const char*>(&r), sizeof r);
        }
        ++positions_;
        entities_ += s.obs.entities.size();
        actions_ += s.actions.size();
        bags_ += s.obs.bags.size();
    }

    uint64_t positions() const { return positions_; }
    uint64_t actions() const { return actions_; }
    const std::string& prefix() const { return prefix_; }

private:
    std::string prefix_;
    std::ofstream pos_, ent_, act_, bag_;
    uint64_t positions_ = 0, entities_ = 0, actions_ = 0, bags_ = 0;
};

namespace detail {
template <class T>
std::vector<T> readTable(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot read " + path);
    const auto bytes = static_cast<size_t>(in.tellg());
    in.seekg(0);
    std::vector<T> rows(bytes / sizeof(T));
    in.read(reinterpret_cast<char*>(rows.data()), static_cast<std::streamsize>(rows.size() * sizeof(T)));
    return rows;
}
} // namespace detail

/// The first `limit` positions of the shard PREFIX (fewer if it is cut
/// short), as the model reads them: tools/nn_eval.cpp and the tests.
inline std::vector<ObsSample> readShard(const std::string& prefix, size_t limit = SIZE_MAX) {
    const auto pos = detail::readTable<PosRecord>(prefix + ".pos");
    const auto ent = detail::readTable<EntRecord>(prefix + ".ent");
    const auto act = detail::readTable<ActRecord>(prefix + ".act");
    const auto bag = detail::readTable<BagRecord>(prefix + ".bag");
    std::vector<ObsSample> out;
    for (size_t p = 0; p < pos.size() && out.size() < limit; ++p) {
        const PosRecord& r = pos[p];
        if (r.ent_start + r.ent_count > ent.size() || r.act_start + r.act_count > act.size() ||
            r.bag_start + r.bag_count > bag.size())
            break;
        ObsSample s;
        s.player = static_cast<PlayerId>(r.player);
        s.turn = r.turn;
        s.kind = r.kind;
        s.chosen = r.chosen;
        s.search_value = r.search_value;
        for (int k = 0; k < kObsGlobalDim; ++k) s.obs.g[k] = r.g[k];
        for (size_t i = 0; i < r.ent_count; ++i) s.obs.entities.push_back(fromRecord(ent[r.ent_start + i]));
        for (size_t i = 0; i < r.act_count; ++i) {
            s.actions.push_back(fromRecord(act[r.act_start + i]));
            s.scores.push_back(act[r.act_start + i].score);
        }
        for (size_t i = 0; i < r.bag_count; ++i) s.obs.bags.emplace_back(bag[r.bag_start + i].bag, bag[r.bag_start + i].card);
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace riftbound::ml
