#pragma once
/// @file equip_base.h
/// Shared equip infrastructure for gear cards — the canonical equip cost
/// payment (canPayEquipCost / payEquipCost), standardEquip, and the
/// SimpleEquipGear / UniversalEquipGear base classes. This is the ONE canonical
/// standardEquip; the old per-audit-file copies redirect here.

#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/effect_executor.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace riftbound {

// ─── Equip cost: [energy] + one Power of `domain` ([A] when nullopt) ─────────
// [Equip] is an activated ability of the gear, so it is paid like any other
// cost (CR 445): from the rune pool first — Power that may only be used on gear
// abilities (Fire Below the Mountain, RunePool::restricted) included — then by
// exhausting runes for Energy and recycling a rune, ready or exhausted, for
// Power (CR 164.2; one rune may do both).
namespace equip_detail {
inline bool runeMatches(const GameObject& rune, std::optional<Domain> domain) {
    if (!domain.has_value()) return true;
    for (auto d : rune.domains) if (d == *domain) return true;
    return false;
}
inline bool poolHasPower(const RunePool& pool, const SpendPurpose& purpose,
                         std::optional<Domain> domain) {
    if (pool.restrictedPowerFor(purpose) > 0 || pool.universal_power > 0) return true;
    return domain.has_value() ? pool.power[static_cast<int>(*domain)] > 0
                              : pool.totalPower() > 0;
}
inline std::vector<GameObjectId> runesInBase(const GameState& state, PlayerId player) {
    std::vector<GameObjectId> out;
    const LocationId base{BaseLocation{player}};
    for (const auto& [id, obj] : state.objects)
        if (obj.isRune() && obj.controller == player && obj.location.has_value() &&
            *obj.location == base) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace equip_detail

inline bool canPayEquipCost(const GameState& state, PlayerId player, int energy_cost,
                            std::optional<Domain> domain) {
    const auto& pool = state.player(player).rune_pool;
    const SpendPurpose purpose = state.purposeAbilityOf(CardType::Gear);
    int ready = 0;
    bool rune_for_power = false;
    for (auto id : equip_detail::runesInBase(state, player)) {
        const auto& rune = state.getObject(id);
        if (!rune.is_exhausted) ++ready;
        if (equip_detail::runeMatches(rune, domain)) rune_for_power = true;
    }
    return ready + pool.energyFor(purpose) >= std::max(0, energy_cost) &&
           (equip_detail::poolHasPower(pool, purpose, domain) || rune_for_power);
}

inline bool payEquipCost(CardContext& ctx, int energy_cost, std::optional<Domain> domain) {
    auto& state = ctx.state;
    const PlayerId player = ctx.controller;
    if (!canPayEquipCost(state, player, energy_cost, domain)) return false;
    auto& ps = state.player(player);
    auto& pool = ps.rune_pool;
    const SpendPurpose purpose = state.purposeAbilityOf(CardType::Gear);

    int e_need = std::max(0, energy_cost);
    pool.spendEnergy(purpose, e_need);
    const bool power_from_pool = equip_detail::poolHasPower(pool, purpose, domain);
    auto runes = equip_detail::runesInBase(state, player);
    if (e_need > 0) {
        // When a rune must pay the Power, exhaust a matching one for Energy
        // first so the same rune can then be recycled.
        std::stable_sort(runes.begin(), runes.end(), [&](GameObjectId a, GameObjectId b) {
            if (power_from_pool) return false;
            return equip_detail::runeMatches(state.getObject(a), domain) >
                   equip_detail::runeMatches(state.getObject(b), domain);
        });
        for (auto id : runes) {
            if (e_need <= 0) break;
            auto& rune = state.getObject(id);
            if (rune.is_exhausted) continue;
            rune.is_exhausted = true;
            ctx.events.emit(ObjectStateChangedEvent{id, "exhausted"});
            --e_need;
        }
    }

    if (power_from_pool) {
        int p_need = 1;
        pool.spendRestrictedPower(purpose, p_need);
        if (p_need > 0 && domain.has_value() && pool.power[static_cast<int>(*domain)] > 0) {
            --pool.power[static_cast<int>(*domain)];
            p_need = 0;
        }
        if (p_need > 0 && !domain.has_value()) {
            for (int di = 0; di < static_cast<int>(Domain::Count) && p_need > 0; ++di)
                if (pool.power[di] > 0) { --pool.power[di]; p_need = 0; }
        }
        if (p_need > 0 && pool.universal_power > 0) { --pool.universal_power; p_need = 0; }
        ctx.events.logTrace("  EQUIP_COST: paid the Power from the rune pool");
        return true;
    }
    // Recycle a matching rune, an exhausted one first (it has nothing else to give).
    GameObjectId pick = kInvalidId;
    for (auto id : runes) {
        const auto& rune = state.getObject(id);
        if (!equip_detail::runeMatches(rune, domain)) continue;
        if (pick == kInvalidId || (rune.is_exhausted && !state.getObject(pick).is_exhausted))
            pick = id;
    }
    if (pick == kInvalidId) return false;   // canPayEquipCost said otherwise
    auto& dr = state.getObject(pick);
    ctx.events.logTrace("  EQUIP_COST: recycled " + dr.name + " for power");
    dr.location = std::nullopt;
    dr.zone = ZoneType::RuneDeck;
    dr.is_exhausted = false;
    ps.rune_deck.insert(ps.rune_deck.begin(), pick);
    ps.power_spent_this_turn++;
    ctx.events.emit(LeftBoardEvent{pick, player, CardType::Rune, BaseLocation{player},
                                   ZoneType::RuneDeck, false});
    return true;
}

// ─── Attach a gear to a unit (cost already paid) ─────────────────────────────
inline void attachEquipment(CardContext& ctx, GameObjectId gear_id, GameObjectId unit_id) {
    auto& gear = ctx.state.getObject(gear_id);
    auto& unit = ctx.state.getObject(unit_id);
    ctx.events.logTrace("EQUIP: " + gear.name + " -> " + unit.name +
                        " (might_bonus=" + std::to_string(gear.might_bonus) + ")");
    gear.attached_to = unit_id;
    unit.attachments.push_back(gear_id);
    gear.location = unit.location;
    gear.zone = unit.zone;
    unit.attachment_might_bonus += gear.might_bonus;
    unit.recomputeMight();
    ctx.events.emit(ObjectStateChangedEvent{gear_id, "attached"});
    ctx.events.emit(ObjectStateChangedEvent{unit_id, "equipped"});
}

// ─── Canonical standard equip: pay [energy] + one [domain] power, then attach ──
inline bool standardEquip(CardContext& ctx, GameObjectId gear_id, GameObjectId unit_id,
                          int energy_cost, std::optional<Domain> domain) {
    if (!ctx.state.objectExists(gear_id) || !ctx.state.objectExists(unit_id)) return false;
    // No state change unless the whole cost can be paid.
    if (!payEquipCost(ctx, energy_cost, domain)) return false;
    attachEquipment(ctx, gear_id, unit_id);
    return true;
}

// ─── Simple equip gear: pay [DOMAIN] power, then attach ─────────────────────
class SimpleEquipGear : public GearCard {
public:
    // Behavior config only (equip domain + energy). The concrete card supplies
    // its data via def().
    explicit SimpleEquipGear(Domain equip_domain, int energy_cost = 0)
        : equip_domain_(equip_domain), energy_cost_(energy_cost) {}

    bool hasEquipAbility() const override { return true; }
    bool onEquip(CardContext& ctx, GameObjectId unit) override {
        return standardEquip(ctx, ctx.source, unit, energy_cost_, equip_domain_);
    }
    bool canEquip(const GameState& state, PlayerId controller) const override {
        return canPayEquipCost(state, controller, energy_cost_, equip_domain_);
    }

protected:
    Domain equip_domain_;
    int energy_cost_;
};

// ─── Universal equip gear: pay [A], then attach ─────────────────────────────
class UniversalEquipGear : public GearCard {
public:
    explicit UniversalEquipGear(int energy_cost = 0)
        : energy_cost_(energy_cost) {}

    bool hasEquipAbility() const override { return true; }
    bool onEquip(CardContext& ctx, GameObjectId unit) override {
        return standardEquip(ctx, ctx.source, unit, energy_cost_, std::nullopt);
    }
    bool canEquip(const GameState& state, PlayerId controller) const override {
        return canPayEquipCost(state, controller, energy_cost_, std::nullopt);
    }

protected:
    int energy_cost_;
};

} // namespace riftbound
