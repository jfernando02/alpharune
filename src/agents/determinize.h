#pragma once
/// @file determinize.h
/// Replace what `me` cannot see with one random guess, in place.
///
/// The look-ahead agent runs its simulations on a copy of the real state.
/// Without this step those simulations would play the opponent's real hand
/// and draw the real next cards, so the agent would plan with information a
/// player at the table does not have.
///
/// What changes:
///   - The opponent's hand and Main Deck are pooled, shuffled and dealt back
///     into the same two piles at the same sizes.
///   - `me`'s own Main Deck is shuffled (its order is unknown to `me`).
///   - During setup, a battlefield the opponent already picked is replaced
///     by a random one of their pool.
/// Whole objects move between the piles, so nothing derived from a card's
/// definition goes stale.
///
/// Known gaps (fix with the knowledge tracker of core/knowledge_view.h):
///   - Cards the opponent revealed earlier are shuffled like any other.
///   - Facedown cards at battlefields are left as they are.

#include "core/game_state.h"

#include <random>

namespace riftbound {

void determinize(GameState& state, PlayerId me, std::mt19937_64& rng);

} // namespace riftbound
