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

class TheBall : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] [Ganking] When you play or move this, choose one — Draw 1. /
    // Deal 2 to enemy units here."
    // It moves only while it is a unit (Lady of Clockwork).
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayThis; }
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayThis, TriggerType::WhenIMove};
    }

    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int m = pickMode(ctx, "The Ball", 2, {"Draw 1", "Deal 2 to enemy units here"});
        if (m == -1) return;
        if (finalizingChoices(ctx)) return;
        if (m == 0) { ctx.executor.drawCards(ctx.controller, 1); return; }
        if (m != 1 || !ctx.state.objectExists(ctx.source)) return;
        const auto hit = enemyUnitsAt(ctx.state, ctx.controller, ctx.state.getObject(ctx.source).battlefieldId());
        for (auto u : hit) ctx.executor.dealDamage(u, 2, ctx.source);
        killLethal(ctx, hit);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1066;
        d.def_id = R"RB(rad-146-167)RB";
        d.name = R"RB(The Ball)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-146/167)RB";
        d.collector_number = 146;
        d.card_type = CardType::Gear;
        d.domains = {Domain::Calm, Domain::Mind};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Ganking);
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
[Ganking] (While this is a unit, it can move from battlefield to battlefield.)
When you play or move this, choose one —Draw 1.Deal 2 to enemy units here.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/4b11f36bd5ce2b08677959664cda3906a04d234c-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1066(CardRegistry& r) {
    r.registerCard(1066, std::make_unique<TheBall>());
}

}  // namespace riftbound
