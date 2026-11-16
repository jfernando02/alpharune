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

class TheGoodStuff : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When you play this, discard 1, then draw 3. When you hold here,
    // return this to its owner's hand."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayThis, TriggerType::WhenYouHoldHere};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger == TriggerType::WhenYouHoldHere) {
            if (ctx.state.objectExists(ctx.source) && ctx.state.getObject(ctx.source).location)
                ctx.executor.bounceToHand(ctx.source);
            return;
        }
        discardOneThenAct(ctx, "The Good Stuff: discard 1", [](CardContext& c, GameObjectId) {
            c.executor.drawCards(c.controller, 3);
        });
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1036;
        d.def_id = R"RB(rad-106-167)RB";
        d.name = R"RB(The Good Stuff)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-106/167)RB";
        d.collector_number = 106;
        d.card_type = CardType::Gear;
        d.domains = {Domain::Body};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When you play this, discard 1, then draw 3.
When you hold here, return this to its owner's hand.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/e7dcc9ad059b471373f0093453a0ca7537d6aa0f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1036(CardRegistry& r) {
    r.registerCard(1036, std::make_unique<TheGoodStuff>());
}

}  // namespace riftbound
