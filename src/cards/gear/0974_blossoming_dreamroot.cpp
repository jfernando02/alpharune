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

class BlossomingDreamroot : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When you win a combat here, draw 1."
    // A combat won here = you conquered here after a combat or held it as
    // defender; approximated by "when you conquer here".
    TriggerType triggerType() const override { return TriggerType::WhenYouConquerHere; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 974;
        d.def_id = R"RB(rad-024-167)RB";
        d.name = R"RB(Blossoming Dreamroot)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-024/167)RB";
        d.collector_number = 24;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Chaos};
        d.energy_cost = 2;
        d.rarity = Rarity::Common;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When you win a combat here, draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5b819cc232389f3ef1438350d68c7801e181961c-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_974(CardRegistry& r) {
    r.registerCard(974, std::make_unique<BlossomingDreamroot>());
}

}  // namespace riftbound
