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

class GravityField : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] Opponents must pay [1][A] to move one or more units to this
    // battlefield. [Deathknell][>] Draw 1."
    // APPROX: the move tax is not modeled.
    TriggerType triggerType() const override { return TriggerType::WhenIDie; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1001;
        d.def_id = R"RB(rad-062-167)RB";
        d.name = R"RB(Gravity Field)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-062/167)RB";
        d.collector_number = 62;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Calm};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Deathknell);
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
Opponents must pay [1][A] to move one or more units to this battlefield.
[Deathknell][>] Draw 1. (When I die, get the effect.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/51f589e5ed0f7316b6cb64ebc93c4ef3e75b7f46-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1001(CardRegistry& r) {
    r.registerCard(1001, std::make_unique<GravityField>());
}

}  // namespace riftbound
