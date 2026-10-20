#include "determinize.h"

#include "core/game_object.h"

#include <algorithm>

namespace riftbound {

void determinize(GameState& state, PlayerId me, std::mt19937_64& rng) {
    PlayerState& theirs = state.player(opponent(me));

    std::vector<GameObjectId> pool;
    pool.reserve(theirs.hand.size() + theirs.main_deck.size());
    pool.insert(pool.end(), theirs.hand.begin(), theirs.hand.end());
    pool.insert(pool.end(), theirs.main_deck.begin(), theirs.main_deck.end());
    std::shuffle(pool.begin(), pool.end(), rng);

    const auto hand_size = static_cast<std::ptrdiff_t>(theirs.hand.size());
    theirs.hand.assign(pool.begin(), pool.begin() + hand_size);
    theirs.main_deck.assign(pool.begin() + hand_size, pool.end());
    for (GameObjectId id : theirs.hand) state.getObject(id).zone = ZoneType::Hand;
    for (GameObjectId id : theirs.main_deck) state.getObject(id).zone = ZoneType::MainDeck;

    auto& my_deck = state.player(me).main_deck;
    std::shuffle(my_deck.begin(), my_deck.end(), rng);
}

} // namespace riftbound
