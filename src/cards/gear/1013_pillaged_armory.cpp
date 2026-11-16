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

class PillagedArmory : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When you play this or when you conquer here, [Buff] a friendly unit."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayThis, TriggerType::WhenYouConquerHere};
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Pillaged Armory: [Buff] a friendly unit", friendlyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t)) ctx.executor.buffUnit(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1013;
        d.def_id = R"RB(rad-073-167)RB";
        d.name = R"RB(Pillaged Armory)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-073/167)RB";
        d.collector_number = 73;
        d.card_type = CardType::Gear;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When you play this or when you conquer here, [Buff] a friendly unit. (Give it a +1 [M] buff if it doesn't have one.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/374248eb65ca1e90a434652073dbbd3a12a11eda-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1013(CardRegistry& r) {
    r.registerCard(1013, std::make_unique<PillagedArmory>());
}

}  // namespace riftbound
