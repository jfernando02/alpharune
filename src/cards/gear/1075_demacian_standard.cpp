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

class DemacianStandard : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When one or more friendly units attack here, give one of them
    // +2 [M] this turn. When you hold here, you may return this to its owner's hand."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouAttackHere, TriggerType::WhenYouHoldHere};
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger == TriggerType::WhenYouHoldHere) {
            const int go = confirmOptional(ctx, "Demacian Standard: return to hand", [] { return true; });
            if (go == -1) return;
            if (finalizingChoices(ctx)) return;
            if (go == 1 && ctx.state.objectExists(ctx.source) && ctx.state.getObject(ctx.source).location)
                ctx.executor.bounceToHand(ctx.source);
            return;
        }
        std::vector<GameObjectId> attackers;
        for (auto u : keepHere(ctx.state, ctx.source, friendlyUnits(ctx.state, ctx.controller)))
            if (ctx.state.getObject(u).combat_designation == CombatDesignation::Attacker) attackers.push_back(u);
        const auto t = pickTarget(ctx, "Demacian Standard: +2 [M] to an attacker", attackers);
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t)) ctx.executor.giveTemporaryMight(t, 2);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1075;
        d.def_id = R"RB(rad-156-167)RB";
        d.name = R"RB(Demacian Standard)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-156/167)RB";
        d.collector_number = 156;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Body, Domain::Order};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When one or more friendly units attack here, give one of them +2 [M] this turn.
When you hold here, you may return this to its owner's hand.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/315b0f7901a83df38b9765f12b1edfdc7de566a3-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1075(CardRegistry& r) {
    r.registerCard(1075, std::make_unique<DemacianStandard>());
}

}  // namespace riftbound
