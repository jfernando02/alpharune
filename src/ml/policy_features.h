#pragma once
/// @file policy_features.h
/// Sparse encoding of one legal action in its position, for the learned
/// policy model (gradient-boosted ranking trees trained in riftbound-trainer
/// on the heuristic agent's search results, read back by
/// agents/policy_model). The model scores every legal action of a decision
/// and the highest score is the policy's pick.
///
///   [0, kValueFeatureDim)         the position, from the acting player's
///                                  point of view (ml::extractValueFeatures)
///   then kPolicyActionGlobalDim   action globals: IntentType and decision
///                                  kind one-hots, the acting card's cost /
///                                  Might / type, targets' and moved units'
///                                  counts and Might, the move destination's
///                                  Might on each side, the choice value,
///                                  whether ScriptedAgent would pick it, ...
///   then 3 card blocks of kCardVocabSize, card_def_id i at offset i-1:
///     the acting card (played, or the ability's source), the chosen
///     objects (targets, chosen objects, a named card), the units moved.
///
/// Changing the layout invalidates trained models: bump kPolicyFeatureVersion.

#include "core/game_state.h"
#include "core/intent.h"
#include "ml/value_features.h"

#include <string>
#include <vector>

namespace riftbound::ml {

inline constexpr int kPolicyFeatureVersion = 1;
inline constexpr int kPolicyActionGlobalDim = 96;
inline constexpr int kPolicyActionOffset = kValueFeatureDim;

enum class PolicyBlock : int { Acting = 0, Chosen, Moved, Count };

inline constexpr int policyBlockOffset(PolicyBlock b) {
    return kPolicyActionOffset + kPolicyActionGlobalDim + static_cast<int>(b) * kCardVocabSize;
}

inline constexpr int kPolicyFeatureDim =
    kPolicyActionOffset + kPolicyActionGlobalDim + static_cast<int>(PolicyBlock::Count) * kCardVocabSize;

/// What kind of decision `legal` is, by the actions on offer — the policy
/// model is used only for kinds it was trained on. One of: main, showdown,
/// chain, damage, mulligan, battlefield, trigger, trigger_cost, rune,
/// choice, other (a string literal).
const char* policyDecisionKind(const GameState& state, const std::vector<Intent>& legal);

/// Encode legal[index] into `out` (cleared first). `position` is
/// extractValueFeatures(state, actor) for the actor legal[index].player,
/// shared by the actions of one decision; `kind` is policyDecisionKind;
/// `scripted_index` the action ScriptedAgent picks (-1 if unknown).
void extractPolicyFeatures(const GameState& state, const CardDB& card_db,
                           const std::vector<Intent>& legal, int index,
                           const SparseRow& position, const char* kind,
                           int scripted_index, SparseRow& out);

/// Human-readable name of every feature index, for reports.
std::vector<std::string> policyFeatureNames(const CardDB& card_db);

} // namespace riftbound::ml
