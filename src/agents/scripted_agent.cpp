#include "scripted_agent.h"

#include "core/game_object.h"

#include <algorithm>
#include <set>
#include <variant>

namespace riftbound {

namespace {

bool isPlay(IntentType t) {
    return t == IntentType::PlayCard || t == IntentType::PlayReaction ||
           t == IntentType::PlayActionCard;
}

bool isActivate(IntentType t) {
    return t == IntentType::ActivateAbility ||
           t == IntentType::ActivateReactionAbility ||
           t == IntentType::ActivateActionAbility;
}

/// Total Might of `player`'s units at battlefield `bf`.
int mightAt(const GameState& state, BattlefieldId bf, PlayerId player) {
    int total = 0;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || obj.controller != player || !obj.location) continue;
        const auto* at = std::get_if<BattlefieldLocation>(&*obj.location);
        if (at && at->id == bf && obj.zone == ZoneType::BattlefieldZone)
            total += obj.current_might;
    }
    return total;
}

} // namespace

int ScriptedAgent::mulliganIndex(const GameState& state,
                                 const std::vector<Intent>& legal) const {
    const auto is_mulligan = [](const Intent& a) {
        return a.type == IntentType::MulliganDecision;
    };
    const auto first = std::find_if(legal.begin(), legal.end(), is_mulligan);
    if (first == legal.end()) return -1;

    // The player going first has 2 runes on turn 1; the player going second
    // channels an extra rune (CR 487.7), so a 3-cost card is a turn-1 play too.
    const PlayerId me = first->player;
    const int max_drop = (me == state.turn.starting_player) ? 2 : 3;
    const auto& hand = state.player(me).hand;
    const auto energy = [&](GameObjectId id) {
        const auto def_id = state.getObject(id).card_def_id;
        return def_id == kInvalidId ? 0 : card_db_.get(def_id).energy_cost;
    };
    const auto cost = [&](GameObjectId id) {
        const auto def_id = state.getObject(id).card_def_id;
        if (def_id == kInvalidId) return 0;
        const auto& def = card_db_.get(def_id);
        return def.energy_cost * 16 + def.power_cost;
    };

    // An early drop is a unit at that cost. A deck with no such unit counts
    // gear and spells at that cost instead. The player knows their own deck
    // list, which at the mulligan is the hand plus the Main Deck.
    const auto is_unit = [&](GameObjectId id) {
        return state.getObject(id).card_type == CardType::Unit;
    };
    const auto at_drop_cost = [&](GameObjectId id) {
        const int e = energy(id);
        return e >= 2 && e <= max_drop;
    };
    const auto& deck = state.player(me).main_deck;
    const auto unit_drop = [&](GameObjectId id) { return is_unit(id) && at_drop_cost(id); };
    const bool deck_has_unit_drops = std::any_of(hand.begin(), hand.end(), unit_drop) ||
                                     std::any_of(deck.begin(), deck.end(), unit_drop);
    const auto is_drop = [&](GameObjectId id) {
        return deck_has_unit_drops ? unit_drop(id) : at_drop_cost(id);
    };

    // Keep any hand with an early drop; otherwise set aside the two most
    // expensive cards, the least likely to be played soon.
    std::vector<GameObjectId> out;
    const bool has_drop = std::any_of(hand.begin(), hand.end(), is_drop);
    if (!has_drop && hand.size() >= 2) {
        out = hand;
        std::stable_sort(out.begin(), out.end(), [&](GameObjectId a, GameObjectId b) {
            return cost(a) > cost(b);
        });
        out.resize(2);
    }

    const std::set<GameObjectId> wanted(out.begin(), out.end());
    for (int i = 0; i < static_cast<int>(legal.size()); ++i) {
        if (!is_mulligan(legal[i])) continue;
        const auto& set_aside = legal[i].cards_to_mulligan;
        if (std::set<GameObjectId>(set_aside.begin(), set_aside.end()) == wanted)
            return i;
    }
    return static_cast<int>(first - legal.begin());
}

int ScriptedAgent::selectIndex(const GameState& state,
                               const std::vector<Intent>& legal) const {
    if (legal.empty()) return -1;
    const int n = static_cast<int>(legal.size());

    // 1. The most expensive playable card (Energy, then Power).
    int best = -1, best_cost = -1;
    for (int i = 0; i < n; ++i) {
        const Intent& a = legal[i];
        if (!isPlay(a.type) || !state.objectExists(a.card)) continue;
        const auto def_id = state.getObject(a.card).card_def_id;
        if (def_id == kInvalidId) continue;
        const auto& def = card_db_.get(def_id);
        const int cost = def.energy_cost * 16 + def.power_cost;
        if (cost > best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    if (best >= 0) return best;

    // 2. Move to the battlefield the opponent holds most weakly; among moves
    //    there, the one that brings the most units.
    int best_opp_might = 0, best_units = 0;
    for (int i = 0; i < n; ++i) {
        const Intent& a = legal[i];
        if (a.type != IntentType::StandardMove || !a.move_destination) continue;
        const auto* to = std::get_if<BattlefieldLocation>(&*a.move_destination);
        if (!to) continue;
        const int opp_might = mightAt(state, to->id, opponent(a.player));
        const int units = static_cast<int>(a.units_to_move.size());
        if (best < 0 || opp_might < best_opp_might ||
            (opp_might == best_opp_might && units > best_units)) {
            best = i;
            best_opp_might = opp_might;
            best_units = units;
        }
    }
    if (best >= 0) return best;

    // 3. An ability.
    for (int i = 0; i < n; ++i)
        if (isActivate(legal[i].type)) return i;

    // 4. Prompts.
    for (int i = 0; i < n; ++i) {
        const Intent& a = legal[i];
        if (a.type == IntentType::PlaceOptionalTrigger ||
            a.type == IntentType::PayTriggeredCost)
            return i;
    }
    for (int i = 0; i < n; ++i) {
        const Intent& a = legal[i];
        // Yes over no; for other integer choices the first is as good as any.
        if (a.type == IntentType::MakeChoice && a.chosen_objects.empty() &&
            a.chosen_value && *a.chosen_value == 1)
            return i;
    }
    const int mulligan = mulliganIndex(state, legal);
    if (mulligan >= 0) return mulligan;
    for (int i = 0; i < n; ++i) {
        const IntentType t = legal[i].type;
        if (t == IntentType::MakeChoice || t == IntentType::AssignCombatDamage ||
            t == IntentType::ChooseBattlefield || t == IntentType::PlayFirstDecision ||
            t == IntentType::MulliganDecision || t == IntentType::HideCard ||
            t == IntentType::DeclineOptionalTrigger ||
            t == IntentType::DeclineTriggeredCost || t == IntentType::SideboardSwap)
            return i;
    }

    // 5. Pass, end the turn, or whatever is left that is not conceding.
    for (int i = 0; i < n; ++i)
        if (legal[i].type != IntentType::Concede) return i;
    return 0;
}

Intent ScriptedAgent::selectAction(const GameState& state,
                                   const std::vector<Intent>& legal_actions) {
    const int i = selectIndex(state, legal_actions);
    if (i < 0) return Intent::concede(state.turn.turn_player);
    return legal_actions[i];
}

} // namespace riftbound
