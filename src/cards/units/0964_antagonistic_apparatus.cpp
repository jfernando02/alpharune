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

class AntagonisticApparatus : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "I enter ready. When you play me, play a ready 3 [M] Mech unit token here."
    bool entersReadyOnPlay() const override { return true; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        createMech(ctx, locationHere(ctx.state, ctx.source, ctx.controller), /*ready=*/true);
        ctx.events.logTrace("ANTAGONISTIC APPARATUS: ready Mech here");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 964;
        d.def_id = R"RB(rad-009-167)RB";
        d.name = R"RB(Antagonistic Apparatus)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-009/167)RB";
        d.collector_number = 9;
        d.artist = R"RB(Caravan Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};
        d.energy_cost = 6;
        d.power_cost = 1;
        d.might = 3;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(I enter ready.
When you play me, play a ready 3 [M] Mech unit token here.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5be775464c8b897501317c6d32fd60c8b962b5c2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_964(CardRegistry& r) {
    r.registerCard(964, std::make_unique<AntagonisticApparatus>());
}

}  // namespace riftbound
