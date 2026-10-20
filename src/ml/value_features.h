#pragma once
/// @file value_features.h
/// Sparse position encoding for the learned value model (gradient-boosted
/// trees trained in riftbound-trainer, read back by agents/gbt_evaluator).
///
/// Encoded from one player's point of view with only what they can see:
///
///   [0, kValueGlobalDim)              globals: points, battlefields held,
///                                      Might, units, hand / deck sizes,
///                                      rune Energy, turn, the hand-written
///                                      evaluate() score, ...
///   then 6 card blocks of kCardVocabSize each, card_def_id i at offset i-1:
///     my base, my battlefields, opponent base, opponent battlefields
///       — units and gear in play, attached gear included, counted per copy;
///     my legend, opponent legend — the deck's identity, 0/1.
///
/// Plain tokens (no card_def_id) are counted only in the token globals.
/// Facedown cards are not encoded. Hand and trash contents are a later
/// addition; the layout appends blocks, so earlier indices stay stable.
///
/// Changing the layout invalidates trained models: bump kValueFeatureVersion.

#include "core/game_state.h"
#include "ml/feature_extractor.h"

#include <string>
#include <utility>
#include <vector>

namespace riftbound::ml {

inline constexpr int kValueFeatureVersion = 1;
inline constexpr int kValueGlobalDim = 32;

enum class ValueBlock : int {
    MyBase = 0,
    MyBattlefields,
    OppBase,
    OppBattlefields,
    MyLegend,
    OppLegend,
    Count,
};

inline constexpr int valueBlockOffset(ValueBlock b) {
    return kValueGlobalDim + static_cast<int>(b) * kCardVocabSize;
}

inline constexpr int kValueFeatureDim =
    kValueGlobalDim + static_cast<int>(ValueBlock::Count) * kCardVocabSize;

/// (index, value) pairs, indices ascending, zeros omitted.
using SparseRow = std::vector<std::pair<int, float>>;

/// Encode `state` from `me`'s point of view into `out` (cleared first).
void extractValueFeatures(const GameState& state, PlayerId me, SparseRow& out);

/// Human-readable name of every feature index (globals by name, card
/// columns as "<block>:<card name> [<id>]"), for reports.
std::vector<std::string> valueFeatureNames(const CardDB& card_db);

} // namespace riftbound::ml
