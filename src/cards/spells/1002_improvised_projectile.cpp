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

// "Choose a friendly gear. Deal damage equal to its Energy cost to a unit at a
// battlefield."
class ImprovisedProjectile : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> friendlyGear(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isGear() && obj.controller == controller && obj.location.has_value())
                out.push_back(id);
        return out;
    }
    static std::vector<GameObjectId> battlefieldUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects) {
            if (!obj.isUnit() || !obj.isAtBattlefield()) continue;
            if (obj.controller != controller && obj.untargetable_by_enemy) continue;
            out.push_back(id);
        }
        return out;
    }
    bool hasLegalTargets(const GameState& state, PlayerId controller) const override {
        return !friendlyGear(state, controller).empty() &&
               !battlefieldUnits(state, controller).empty();
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 2};
    }
    bool needsPlayTimeTargetPair() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [gear, target] = pickTargetPair(
            ctx, "Improvised Projectile: choose a friendly gear, then a unit at a battlefield",
            friendlyGear(ctx.state, ctx.controller),
            [&](GameObjectId) { return battlefieldUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;  // waiting for a pick
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (gear == kInvalidId || target == kInvalidId) return;
        if (!ctx.state.objectExists(gear) || !ctx.state.objectExists(target)) return;
        if (!ctx.state.getObject(target).isAtBattlefield()) return;
        const auto& g = ctx.state.getObject(gear);
        const int damage = g.card_def_id == kInvalidId
            ? 0 : ctx.executor.cardDB().get(g.card_def_id).energy_cost;
        if (damage <= 0) return;
        ctx.executor.dealDamage(target, damage, ctx.source);
        if (ctx.state.objectExists(target) && ctx.state.getObject(target).hasLethalDamage())
            ctx.executor.killObject(target);
        ctx.events.logTrace("IMPROVISED PROJECTILE: deal " + std::to_string(damage));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1002;
        d.def_id = R"RB(rad-056-167)RB";
        d.name = R"RB(Improvised Projectile)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-056/167)RB";
        d.collector_number = 56;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Choose a friendly gear. Deal damage equal to its Energy cost to a unit at a battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ecbb970f955b8e12369921f30ad08cab04e59a3f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1002(CardRegistry& r) {
    r.registerCard(1002, std::make_unique<ImprovisedProjectile>());
}

} // namespace riftbound
