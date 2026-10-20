#include "evaluator.h"

#include "core/game_object.h"

#include <algorithm>
#include <variant>

namespace riftbound {

namespace {

/// Total Might of `player`'s units at battlefield `bf`.
int mightAtBattlefield(const GameState& state, BattlefieldId bf, PlayerId player) {
    int total = 0;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || obj.controller != player || !obj.location) continue;
        if (obj.zone != ZoneType::BattlefieldZone) continue;
        const auto* at = std::get_if<BattlefieldLocation>(&*obj.location);
        if (at && at->id == bf) total += obj.current_might;
    }
    return total;
}

int boardMight(const GameState& state, PlayerId player) {
    int total = 0;
    for (GameObjectId id : state.allUnitsControlledBy(player))
        total += state.getObject(id).current_might;
    return total;
}

/// Distinct cards in `player`'s trash that a standing grant lets them play.
int trashReplayable(const GameState& state, PlayerId player) {
    std::vector<GameObjectId> cards;
    for (const auto& grant : state.player(player).trash_replay_grants) {
        if (!state.objectExists(grant.card)) continue;
        if (state.getObject(grant.card).zone != ZoneType::Trash) continue;
        if (std::find(cards.begin(), cards.end(), grant.card) == cards.end())
            cards.push_back(grant.card);
    }
    return static_cast<int>(cards.size());
}

/// Energy `player`'s runes give over the next `horizon` turns if they
/// channel 2 a turn into base, up to every rune they own (base + rune deck).
int projectedRuneEnergy(const GameState& state, PlayerId player, int horizon) {
    const int in_base = static_cast<int>(state.runesInBase(player).size());
    const int owned = in_base + static_cast<int>(state.player(player).rune_deck.size());
    int total = 0;
    for (int t = 1; t <= horizon; ++t) total += std::min(owned, in_base + 2 * t);
    return total;
}

} // namespace

bool setWeight(EvalWeights& w, const std::string& name, double value) {
    if (name == "points") w.points = value;
    else if (name == "controlled_bf") w.controlled_bf = value;
    else if (name == "contested_might") w.contested_might = value;
    else if (name == "board_might") w.board_might = value;
    else if (name == "hand") w.hand = value;
    else if (name == "trash_replayable") w.trash_replayable = value;
    else if (name == "runes") w.runes = value;
    else if (name == "rune_energy") w.rune_energy = value;
    else if (name == "rune_horizon") w.rune_horizon = static_cast<int>(value);
    else if (name == "might_cap") w.might_cap = static_cast<int>(value);
    else return false;
    return true;
}

double evaluate(const GameState& state, PlayerId me, const EvalWeights& w) {
    const PlayerId them = opponent(me);

    if (state.game_over) {
        if (state.winner == me) return kWinScore;
        if (state.winner == them) return -kWinScore;
        return 0.0;
    }

    const PlayerState& mine = state.player(me);
    const PlayerState& theirs = state.player(them);

    double score = w.points * (mine.score - theirs.score);

    for (const auto& bf : state.battlefields) {
        if (bf.controller == me) score += w.controlled_bf;
        else if (bf.controller == them) score -= w.controlled_bf;

        const int lead = mightAtBattlefield(state, bf.id, me) -
                         mightAtBattlefield(state, bf.id, them);
        score += w.contested_might * std::clamp(lead, -w.might_cap, w.might_cap);
    }

    score += w.board_might * (boardMight(state, me) - boardMight(state, them));
    score += w.hand * (static_cast<double>(mine.hand.size()) -
                       static_cast<double>(theirs.hand.size()));
    score += w.trash_replayable * (trashReplayable(state, me) - trashReplayable(state, them));
    score += w.runes * (static_cast<double>(state.runesInBase(me).size()) -
                        static_cast<double>(state.runesInBase(them).size()));
    if (w.rune_energy != 0.0)
        score += w.rune_energy * (projectedRuneEnergy(state, me, w.rune_horizon) -
                                  projectedRuneEnergy(state, them, w.rune_horizon));
    return score;
}

} // namespace riftbound
