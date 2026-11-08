#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "As an additional cost to play this, return a friendly unit to its owner's
// hand. Deal 5 to a unit at a battlefield." The returned unit and the target
// are chosen when the spell is finalized, and the return is paid then.
class ParallelConvergence : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> friendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value())
                out.push_back(id);
        return out;
    }
    static std::vector<GameObjectId> battlefieldUnits(const GameState& state, PlayerId controller,
                                                      GameObjectId except) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects) {
            if (id == except || !obj.isUnit() || !obj.isAtBattlefield()) continue;
            if (obj.controller != controller && obj.untargetable_by_enemy) continue;
            out.push_back(id);
        }
        return out;
    }
    bool canBePlayed(const GameState& state, PlayerId player) const override {
        return !friendlyUnits(state, player).empty();
    }
    // A unit to return, and a unit at a battlefield other than that one.
    bool hasLegalTargets(const GameState& state, PlayerId controller) const override {
        for (GameObjectId a : friendlyUnits(state, controller))
            if (!battlefieldUnits(state, controller, a).empty()) return true;
        return false;
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 2, .must_be_unit = true};
    }
    bool needsPlayTimeTargetPair() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [returned, target] = pickTargetPair(
            ctx, "Parallel Convergence: return a friendly unit, then deal 5",
            costChoices(ctx, friendlyUnits(ctx.state, ctx.controller)),
            [&](GameObjectId a) { return battlefieldUnits(ctx.state, ctx.controller, a); });
        if (ctx.executor.hasPendingChoice()) return;  // waiting for a pick
        if (returned != kInvalidId && ctx.state.objectExists(returned) && costDueNow(ctx))
            ctx.executor.bounceToHand(returned);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (returned == kInvalidId && !costAlreadyPaid(ctx)) return;  // the cost was never paid
        if (target == kInvalidId || !ctx.state.objectExists(target)) return;
        if (!ctx.state.getObject(target).isAtBattlefield()) return;
        ctx.executor.dealDamage(target, 5, ctx.source);
        if (ctx.state.objectExists(target) && ctx.state.getObject(target).hasLethalDamage())
            ctx.executor.killObject(target);
        ctx.events.logTrace("PARALLEL CONVERGENCE: returned a friendly unit -> deal 5");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 999;
        d.def_id = R"RB(rad-051-167)RB";
        d.name = R"RB(Parallel Convergence)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-051/167)RB";
        d.collector_number = 51;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(As an additional cost to play this, return a friendly unit to its owner's hand.
Deal 5 to a unit at a battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/55e382346c6fa890936a1cf14abaf57b8ee0b8f2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_999(CardRegistry& r) {
    r.registerCard(999, std::make_unique<ParallelConvergence>());
}

} // namespace riftbound
