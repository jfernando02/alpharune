#include "policy_features.h"

#include "core/card_db.h"
#include "core/game_object.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace riftbound::ml {

namespace {

constexpr int kIntentTypeSlots = 24;
constexpr const char* kKinds[] = {
    "main", "showdown", "chain", "damage", "mulligan", "battlefield",
    "trigger", "trigger_cost", "rune", "choice", "other",
};
constexpr int kKindCount = static_cast<int>(std::size(kKinds));
static_assert(static_cast<int>(IntentType::Concede) < kIntentTypeSlots);

// Action globals after the IntentType and kind one-hots, in index order.
constexpr const char* kScalarNames[] = {
    "acting_energy_cost", "acting_power_cost", "acting_might",    "acting_is_unit",
    "acting_is_spell",    "acting_is_gear",    "acting_is_mine",  "play_to_battlefield",
    "play_source",        "alt_play_cost",     "pay_location_cost", "ability_index",
    "chosen_count",       "chosen_mine",       "chosen_opp",      "chosen_my_might",
    "chosen_opp_might",   "chosen_damaged",    "moved_count",     "moved_might",
    "dest_is_battlefield", "dest_my_might",    "dest_opp_might",  "dest_controller",
    "has_chosen_value",   "chosen_value",      "mulligan_count",  "damage_total",
    "damage_lethal",      "is_scripted_pick",  "legal_count",
    // Appended (slots read 0 in older models, so they stay valid).
    "acting_temporary",   "chosen_temporary",  "moved_temporary", "dest_my_units",
    "dest_opp_units",     "chosen_gear_count",
};
constexpr int kScalarOffset = kIntentTypeSlots + kKindCount;
static_assert(kScalarOffset + static_cast<int>(std::size(kScalarNames)) <= kPolicyActionGlobalDim);

enum Scalar : int {
    ActingEnergyCost, ActingPowerCost, ActingMight, ActingIsUnit,
    ActingIsSpell, ActingIsGear, ActingIsMine, PlayToBattlefield,
    PlaySource, AltPlayCost, PayLocationCost, AbilityIndex,
    ChosenCount, ChosenMine, ChosenOpp, ChosenMyMight,
    ChosenOppMight, ChosenDamaged, MovedCount, MovedMight,
    DestIsBattlefield, DestMyMight, DestOppMight, DestController,
    HasChosenValue, ChosenValue, MulliganCount, DamageTotal,
    DamageLethal, IsScriptedPick, LegalCount,
    ActingTemporary, ChosenTemporary, MovedTemporary, DestMyUnits,
    DestOppUnits, ChosenGearCount,
};

constexpr const char* kBlockNames[] = {"acting", "chosen", "moved"};
static_assert(std::size(kBlockNames) == static_cast<size_t>(PolicyBlock::Count));

bool inVocab(CardDefId id) { return id != kInvalidId && id >= 1 && id <= kCardVocabSize; }

bool offers(const std::vector<Intent>& legal, IntentType t) {
    return std::any_of(legal.begin(), legal.end(), [t](const Intent& a) { return a.type == t; });
}

const GameObject* find(const GameState& state, GameObjectId id) {
    return id != kInvalidId && state.objectExists(id) ? &state.getObject(id) : nullptr;
}

} // namespace

const char* policyDecisionKind(const GameState& state, const std::vector<Intent>& legal) {
    if (offers(legal, IntentType::EndTurn)) return "main";
    if (offers(legal, IntentType::PassFocus)) return "showdown";
    if (offers(legal, IntentType::PassPriority)) return "chain";
    if (offers(legal, IntentType::AssignCombatDamage)) return "damage";
    if (offers(legal, IntentType::MulliganDecision)) return "mulligan";
    if (offers(legal, IntentType::ChooseBattlefield)) return "battlefield";
    if (offers(legal, IntentType::PlaceOptionalTrigger) || offers(legal, IntentType::DeclineOptionalTrigger))
        return "trigger";
    if (offers(legal, IntentType::PayTriggeredCost) || offers(legal, IntentType::DeclineTriggeredCost))
        return "trigger_cost";
    if (offers(legal, IntentType::MakeChoice)) return state.cost_cursor.has_value() ? "rune" : "choice";
    return "other";
}

void extractPolicyFeatures(const GameState& state, const CardDB& card_db,
                           const std::vector<Intent>& legal, int index,
                           const SparseRow& position, const char* kind,
                           int scripted_index, SparseRow& out) {
    out = position;
    const Intent& a = legal[index];
    const PlayerId me = a.player;

    std::array<float, kPolicyActionGlobalDim> g{};
    g[static_cast<int>(a.type)] = 1.0f;
    for (int k = 0; k < kKindCount; ++k)
        if (std::strcmp(kKinds[k], kind) == 0) g[kIntentTypeSlots + k] = 1.0f;
    const auto set = [&](Scalar s, float v) { g[kScalarOffset + s] = v; };
    const auto add = [&](Scalar s, float v) { g[kScalarOffset + s] += v; };

    std::vector<std::pair<int, float>> cards;
    const auto card = [&](PolicyBlock block, CardDefId def) {
        if (inVocab(def)) cards.emplace_back(policyBlockOffset(block) + def - 1, 1.0f);
    };

    // The acting card: the one played or hidden, or the ability's source.
    const GameObjectId acting_id = a.card != kInvalidId ? a.card : a.ability_source;
    if (const GameObject* acting = find(state, acting_id)) {
        card(PolicyBlock::Acting, acting->card_def_id);
        if (inVocab(acting->card_def_id)) {
            const auto it = card_db.all().find(acting->card_def_id);
            if (it != card_db.all().end()) {
                set(ActingEnergyCost, static_cast<float>(it->second.energy_cost));
                set(ActingPowerCost, static_cast<float>(it->second.power_cost));
            }
        }
        set(ActingMight, static_cast<float>(acting->current_might));
        set(ActingIsUnit, acting->isUnit() ? 1.0f : 0.0f);
        set(ActingIsSpell, acting->isSpell() ? 1.0f : 0.0f);
        set(ActingIsGear, acting->isGear() ? 1.0f : 0.0f);
        set(ActingIsMine, acting->controller == me ? 1.0f : 0.0f);
        set(ActingTemporary, acting->hasKeyword(Keyword::Temporary) ? 1.0f : 0.0f);
    }
    if (a.play_location && std::holds_alternative<BattlefieldLocation>(*a.play_location))
        set(PlayToBattlefield, 1.0f);
    set(PlaySource, static_cast<float>(a.play_source));
    set(AltPlayCost, a.use_alt_play_cost ? 1.0f : 0.0f);
    set(PayLocationCost, a.pay_location_cost ? 1.0f : 0.0f);
    set(AbilityIndex, static_cast<float>(a.ability_index));
    card(PolicyBlock::Acting, a.granted_ability_def);

    // Chosen objects: targets, objects chosen in a prompt (runes paid with,
    // units picked), a named card.
    const auto chosen = [&](GameObjectId id) {
        const GameObject* o = find(state, id);
        add(ChosenCount, 1.0f);
        if (!o) return;
        card(PolicyBlock::Chosen, o->card_def_id);
        const bool mine = o->controller == me;
        add(mine ? ChosenMine : ChosenOpp, 1.0f);
        if (o->isUnit()) add(mine ? ChosenMyMight : ChosenOppMight, static_cast<float>(o->current_might));
        if (o->damage_marked > 0) add(ChosenDamaged, 1.0f);
        if (o->hasKeyword(Keyword::Temporary)) add(ChosenTemporary, 1.0f);
        add(ChosenGearCount, static_cast<float>(o->attachments.size()));
    };
    for (GameObjectId id : a.targets) chosen(id);
    for (GameObjectId id : a.chosen_objects) chosen(id);
    card(PolicyBlock::Chosen, a.chosen_card_def);

    for (GameObjectId id : a.units_to_move) {
        add(MovedCount, 1.0f);
        if (const GameObject* o = find(state, id)) {
            card(PolicyBlock::Moved, o->card_def_id);
            add(MovedMight, static_cast<float>(o->current_might));
            if (o->hasKeyword(Keyword::Temporary)) add(MovedTemporary, 1.0f);
        }
    }
    if (a.move_destination && std::holds_alternative<BattlefieldLocation>(*a.move_destination)) {
        const BattlefieldId bf = std::get<BattlefieldLocation>(*a.move_destination).id;
        set(DestIsBattlefield, 1.0f);
        for (const auto& [id, o] : state.objects) {
            if (!o.isUnit() || !o.location || o.zone != ZoneType::BattlefieldZone) continue;
            const auto* at = std::get_if<BattlefieldLocation>(&*o.location);
            if (!at || at->id != bf) continue;
            add(o.controller == me ? DestMyMight : DestOppMight, static_cast<float>(o.current_might));
            add(o.controller == me ? DestMyUnits : DestOppUnits, 1.0f);
        }
        for (const auto& b : state.battlefields)
            if (b.id == bf && b.controller) set(DestController, *b.controller == me ? 1.0f : -1.0f);
    }

    if (a.chosen_value) {
        set(HasChosenValue, 1.0f);
        set(ChosenValue, static_cast<float>(*a.chosen_value));
    }
    set(MulliganCount, static_cast<float>(a.cards_to_mulligan.size()));
    for (const auto& d : a.damage_assignments) {
        add(DamageTotal, static_cast<float>(d.damage));
        if (const GameObject* o = find(state, d.target_unit))
            if (o->damage_marked + d.damage >= o->current_might) add(DamageLethal, 1.0f);
    }
    set(IsScriptedPick, index == scripted_index ? 1.0f : 0.0f);
    set(LegalCount, static_cast<float>(legal.size()));

    for (int i = 0; i < kPolicyActionGlobalDim; ++i)
        if (g[i] != 0.0f) out.emplace_back(kPolicyActionOffset + i, g[i]);
    std::sort(cards.begin(), cards.end());
    for (const auto& [i, v] : cards) {
        if (!out.empty() && out.back().first == i) out.back().second += v;
        else out.emplace_back(i, v);
    }
}

std::vector<std::string> policyFeatureNames(const CardDB& card_db) {
    std::vector<std::string> names = valueFeatureNames(card_db);
    names.resize(kPolicyFeatureDim);
    for (int i = 0; i < kPolicyActionGlobalDim; ++i) {
        std::string n;
        if (i < kIntentTypeSlots)
            n = i <= static_cast<int>(IntentType::Concede)
                    ? std::string("type_") + toString(static_cast<IntentType>(i))
                    : "reserved_type_" + std::to_string(i);
        else if (i < kScalarOffset)
            n = std::string("kind_") + kKinds[i - kIntentTypeSlots];
        else if (i - kScalarOffset < static_cast<int>(std::size(kScalarNames)))
            n = kScalarNames[i - kScalarOffset];
        else
            n = "reserved_action_" + std::to_string(i);
        names[kPolicyActionOffset + i] = n;
    }
    for (int b = 0; b < static_cast<int>(PolicyBlock::Count); ++b) {
        for (int c = 1; c <= kCardVocabSize; ++c) {
            const auto it = card_db.all().find(c);
            const std::string card = it != card_db.all().end() ? it->second.name : "unused";
            names[policyBlockOffset(static_cast<PolicyBlock>(b)) + c - 1] =
                std::string(kBlockNames[b]) + ":" + card + " [" + std::to_string(c) + "]";
        }
    }
    return names;
}

} // namespace riftbound::ml
