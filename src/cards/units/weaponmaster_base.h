#pragma once
/// @file weaponmaster_base.h
/// [Weaponmaster] (CR 821): the shared trigger and the WeaponmasterUnit base.

#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/effect_executor.h"

#include <algorithm>
#include <string>
#include <vector>

namespace riftbound {

namespace weaponmaster_detail {
inline const CardRegistry* registry(const CardContext& ctx) {
    return ctx.registry ? ctx.registry : ctx.executor.cardRegistry();
}
inline bool isEquipment(const GameObject& obj) {
    return std::find(obj.tags.begin(), obj.tags.end(), "Equipment") != obj.tags.end();
}
// The gear's own Card answers what its Equip cost reduced by [A] is (Card::weaponmasterEquip).
inline bool equip(CardContext& ctx, GameObjectId gear, bool check_only) {
    const CardRegistry* reg = registry(ctx);
    if (!reg) return false;
    Card* gc = reg->get(ctx.state.getObject(gear).card_def_id);
    if (!gc) return false;
    CardContext gctx{ctx.state, ctx.events, ctx.executor, ctx.controller, gear};
    gctx.registry = reg;
    return gc->weaponmasterEquip(gctx, ctx.source, check_only);
}
}  // namespace weaponmaster_detail

/// Equipment the Weaponmaster unit `ctx.source` can take now: gear `ctx.controller` controls
/// on the board with the Equipment tag, attached to another unit or to nothing, whose Equip
/// cost reduced by [A] they can pay. Choosing one that can't be paid does nothing
/// (CR 821.1.c.5), so it is not offered. Empty once the Weaponmaster unit left the board.
inline std::vector<GameObjectId> weaponmasterCandidates(CardContext& ctx) {
    std::vector<GameObjectId> out;
    if (!ctx.state.objectExists(ctx.source)) return out;
    const auto& me = ctx.state.getObject(ctx.source);
    if (!me.isUnit() || !me.location.has_value()) return out;
    for (const auto& [id, obj] : ctx.state.objects) {
        if (!obj.isGear() || obj.controller != ctx.controller || !obj.location.has_value()) continue;
        if (!weaponmaster_detail::isEquipment(obj)) continue;
        if (obj.attached_to.has_value() && *obj.attached_to == ctx.source) continue;
        out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::remove_if(out.begin(), out.end(), [&](GameObjectId id) {
                  return !weaponmaster_detail::equip(ctx, id, /*check_only=*/true);
              }), out.end());
    return out;
}

/// The Weaponmaster choices (CR 821.1.c): "you may choose a card you control with the
/// Equipment tag". Returns the chosen Equipment, or kInvalidId when declined, when there is
/// none, or while waiting for an answer (then ctx.executor.hasPendingChoice() is true).
/// Uses confirmOptional and pickTarget.
inline GameObjectId chooseWeaponmasterEquipment(Card& self, CardContext& ctx, const std::string& name) {
    const auto cands = weaponmasterCandidates(ctx);
    const int conf = self.confirmOptional(ctx, name + ": [Weaponmaster] equip an Equipment to me?",
                                          [&cands] { return !cands.empty(); });
    if (conf != 1) return kInvalidId;   // declined, none, or waiting for the answer
    return self.pickTarget(ctx, name + ": [Weaponmaster] which Equipment?", cands);
}

/// Pays the chosen Equipment's Equip cost reduced by [A] and attaches it (CR 821.1.c.3-5).
inline void applyWeaponmaster(CardContext& ctx, GameObjectId picked, const std::string& name) {
    if (picked == kInvalidId || !ctx.state.objectExists(picked)) return;
    if (!weaponmaster_detail::equip(ctx, picked, /*check_only=*/false)) return;
    ctx.events.logTrace(name + ": [Weaponmaster] equipped " + ctx.state.getObject(picked).name);
}

/// The Weaponmaster trigger, CR 821.1.c: "When you play me, you may choose a card you control
/// with the Equipment tag. Pay the cost of its Equip ability, reduced by [A], to attach it to
/// this unit" -- even one attached to another unit, which then moves over. The choices are
/// made when the trigger is finalized: the calling card's choicesAtFinalization() must include
/// kFinalizeTrigger. This ends with the finalizingChoices marker.
inline void resolveWeaponmaster(Card& self, CardContext& ctx, const std::string& name) {
    const GameObjectId picked = chooseWeaponmasterEquipment(self, ctx, name);
    if (ctx.executor.hasPendingChoice()) return;   // waiting for the answer
    if (Card::finalizingChoices(ctx)) return;      // choices end here (CR 383.3)
    applyWeaponmaster(ctx, picked, name);
}

/// A unit whose only triggered ability is [Weaponmaster].
class WeaponmasterUnit : public UnitCard {
public:
    // Abstract: concrete weaponmaster cards supply their data via def().
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        resolveWeaponmaster(*this, ctx, def().name);
    }
};

} // namespace riftbound
