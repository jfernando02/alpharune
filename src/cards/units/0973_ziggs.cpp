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

class ZiggsShortFused : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, play a Bomb gear token to a battlefield. Abilities of
    // friendly gear deal 1 Bonus Damage." (the Bonus Damage: the Bomb token)
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        playBombTrigger(*this, ctx, "ZIGGS");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 973;
        d.def_id = R"RB(rad-023-167)RB";
        d.name = R"RB(Ziggs, Short-Fused)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-023/167)RB";
        d.collector_number = 23;
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Fury};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 5;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(When you play me, play a Bomb gear token to a battlefield. (It has [Deploy]. When it dies, deal 2 to an enemy there and kill your other Bombs there.)
Abilities of friendly gear deal 1 Bonus Damage.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9df0d347bcfc0593cb724258ee07a1208c620848-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_973(CardRegistry& r) {
    r.registerCard(973, std::make_unique<ZiggsShortFused>());
}

}  // namespace riftbound
