#include "value_features.h"

#include "agents/evaluator.h"
#include "core/card_db.h"
#include "core/game_object.h"

#include <algorithm>
#include <array>

namespace riftbound::ml {

namespace {

// Global feature names, in index order. Unlisted indices up to
// kValueGlobalDim are reserved (always 0).
constexpr std::array<const char*, 32> kGlobalNames = {
    "my_points",          "opp_points",         "victory_score",
    "my_bf_held",         "opp_bf_held",        "my_board_might",
    "opp_board_might",    "my_bf_might",        "opp_bf_might",
    "my_units",           "opp_units",          "my_token_units",
    "opp_token_units",    "my_hand_size",       "opp_hand_size",
    "my_deck_size",       "opp_deck_size",      "my_runes_in_base",
    "opp_runes_in_base",  "my_rune_energy",     "opp_rune_energy",
    "my_trash_replayable", "opp_trash_replayable", "turn_number",
    "is_my_turn",         "i_went_first",       "my_xp",
    "opp_xp",             "hand_eval",          "my_trash_size",
    "opp_trash_size",     "battlefield_count",
};
static_assert(kGlobalNames.size() <= kValueGlobalDim);

constexpr const char* kBlockNames[] = {
    "my_base", "my_bf", "opp_base", "opp_bf", "my_legend", "opp_legend",
};
static_assert(std::size(kBlockNames) == static_cast<size_t>(ValueBlock::Count));

bool inVocab(CardDefId id) { return id != kInvalidId && id >= 1 && id <= kCardVocabSize; }

} // namespace

void extractValueFeatures(const GameState& state, PlayerId me, SparseRow& out) {
    out.clear();
    const PlayerId opp = opponent(me);
    const PlayerState& mine = state.player(me);
    const PlayerState& theirs = state.player(opp);

    std::array<float, kValueGlobalDim> g{};
    g[0] = static_cast<float>(mine.score);
    g[1] = static_cast<float>(theirs.score);
    g[2] = static_cast<float>(state.mode.victory_score);
    for (const auto& bf : state.battlefields) {
        if (bf.controller == me) g[3] += 1;
        else if (bf.controller == opp) g[4] += 1;
    }

    std::vector<std::pair<int, float>> cards;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.location || obj.is_hidden) continue;
        if (obj.zone != ZoneType::Base && obj.zone != ZoneType::BattlefieldZone) continue;
        if (!obj.isUnit() && !obj.isGear()) continue;
        if (obj.controller != me && obj.controller != opp) continue;
        const bool is_mine = obj.controller == me;
        const bool at_bf = obj.zone == ZoneType::BattlefieldZone;

        if (obj.isUnit()) {
            g[is_mine ? 5 : 6] += static_cast<float>(obj.current_might);
            if (at_bf) g[is_mine ? 7 : 8] += static_cast<float>(obj.current_might);
            g[is_mine ? 9 : 10] += 1;
            if (obj.is_token) g[is_mine ? 11 : 12] += 1;
        }
        if (inVocab(obj.card_def_id)) {
            const ValueBlock block = is_mine ? (at_bf ? ValueBlock::MyBattlefields : ValueBlock::MyBase)
                                             : (at_bf ? ValueBlock::OppBattlefields : ValueBlock::OppBase);
            cards.emplace_back(valueBlockOffset(block) + obj.card_def_id - 1, 1.0f);
        }
    }

    g[13] = static_cast<float>(mine.hand.size());
    g[14] = static_cast<float>(theirs.hand.size());
    g[15] = static_cast<float>(mine.main_deck.size());
    g[16] = static_cast<float>(theirs.main_deck.size());
    g[17] = static_cast<float>(state.runesInBase(me).size());
    g[18] = static_cast<float>(state.runesInBase(opp).size());
    const EvalWeights defaults{};
    g[19] = static_cast<float>(projectedRuneEnergy(state, me, defaults.rune_horizon));
    g[20] = static_cast<float>(projectedRuneEnergy(state, opp, defaults.rune_horizon));
    g[21] = static_cast<float>(trashReplayable(state, me));
    g[22] = static_cast<float>(trashReplayable(state, opp));
    g[23] = static_cast<float>(state.turn.turn_number);
    g[24] = state.turn.turn_player == me ? 1.0f : 0.0f;
    g[25] = state.turn.starting_player == me ? 1.0f : 0.0f;
    g[26] = static_cast<float>(mine.xp);
    g[27] = static_cast<float>(theirs.xp);
    g[28] = static_cast<float>(evaluate(state, me, defaults));
    g[29] = static_cast<float>(mine.trash.size());
    g[30] = static_cast<float>(theirs.trash.size());
    g[31] = static_cast<float>(state.battlefields.size());

    for (int i = 0; i < kValueGlobalDim; ++i)
        if (g[i] != 0.0f) out.emplace_back(i, g[i]);

    const auto legend = [&](const PlayerState& ps, ValueBlock block) {
        if (ps.legend_zone == kInvalidId || !state.objectExists(ps.legend_zone)) return;
        const CardDefId def = state.getObject(ps.legend_zone).card_def_id;
        if (inVocab(def)) cards.emplace_back(valueBlockOffset(block) + def - 1, 1.0f);
    };
    legend(mine, ValueBlock::MyLegend);
    legend(theirs, ValueBlock::OppLegend);

    // Copies of a card add up.
    std::sort(cards.begin(), cards.end());
    for (const auto& [index, value] : cards) {
        if (!out.empty() && out.back().first == index) out.back().second += value;
        else out.emplace_back(index, value);
    }
}

std::vector<std::string> valueFeatureNames(const CardDB& card_db) {
    std::vector<std::string> names(kValueFeatureDim);
    for (int i = 0; i < kValueGlobalDim; ++i)
        names[i] = i < static_cast<int>(kGlobalNames.size()) ? kGlobalNames[i]
                                                             : "reserved_" + std::to_string(i);
    for (int b = 0; b < static_cast<int>(ValueBlock::Count); ++b) {
        for (int c = 1; c <= kCardVocabSize; ++c) {
            const auto it = card_db.all().find(c);
            const std::string card = it != card_db.all().end() ? it->second.name : "unused";
            names[valueBlockOffset(static_cast<ValueBlock>(b)) + c - 1] =
                std::string(kBlockNames[b]) + ":" + card + " [" + std::to_string(c) + "]";
        }
    }
    return names;
}

} // namespace riftbound::ml
