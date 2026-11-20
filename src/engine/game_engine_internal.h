#pragma once

// File-scope helpers shared by more than one game_engine_*.cpp file.

#include "engine/game_engine.h"

namespace riftbound {

// A "once each turn, you may pay less" discount already used this turn.
inline bool onceDiscountSpent(const GameState& state, const PlayerState::CostModifier& mod) {
    if (!state.objectExists(mod.source)) return true;
    const auto& c = state.getObject(mod.source).card_counters;
    auto it = c.find("__discount_turn");
    return it != c.end() && it->second == state.turn.turn_number;
}

}  // namespace riftbound
