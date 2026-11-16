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

class FirelightsHideout : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you conquer here, the next unit you play this turn has [Accelerate]."
    TriggerType triggerType() const override { return TriggerType::WhenYouConquerHere; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.state.player(ctx.controller).next_unit_accelerate = true;
        ctx.events.logTrace("FIRELIGHTS' HIDEOUT: the next unit played this turn has [Accelerate]");
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1081;
        d.def_id = R"RB(rad-162-167)RB";
        d.name = R"RB(Firelights' Hideout)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-162/167)RB";
        d.collector_number = 162;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you conquer here, the next unit you play this turn has [Accelerate]. (You may pay [1] and a Power of its Domain as an additional cost to have it enter ready.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/f8e2d555ff0dc250de38a1c8924a199dc121f036-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1081(CardRegistry& r) {
    r.registerCard(1081, std::make_unique<FirelightsHideout>());
}

}  // namespace riftbound
