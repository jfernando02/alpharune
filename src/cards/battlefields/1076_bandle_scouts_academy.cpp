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

class BandleScoutsAcademy : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you hide a card here, play a Gold gear token exhausted."
    TriggerType triggerType() const override { return TriggerType::WhenYouHideACard; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        createGoldExhausted(ctx, ctx.controller);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1076;
        d.def_id = R"RB(rad-157-167)RB";
        d.name = R"RB(Bandle Scouts' Academy)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-157/167)RB";
        d.collector_number = 157;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you hide a card here, play a Gold gear token exhausted. (It has "[Reaction][>] Kill this, [E]: [Add] [A]."))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/f5bb93e1ad9005fd42a9129b91f72877f1dedbe1-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1076(CardRegistry& r) {
    r.registerCard(1076, std::make_unique<BandleScoutsAcademy>());
}

}  // namespace riftbound
