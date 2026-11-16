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

class EntrenchedBoar : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "You may have me enter ready. If you do, I can't move this turn."
    bool entersReadyOnPlay() const override { return true; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        ctx.state.getObject(ctx.source).cant_move_on_turn = ctx.state.turn.turn_number;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 975;
        d.def_id = R"RB(rad-026-167)RB";
        d.name = R"RB(Entrenched Boar)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-026/167)RB";
        d.collector_number = 26;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 2;
        d.might = 2;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(You may have me enter ready. If you do, I can't move this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/8d8688f43e219f7f4b5f5fd17cff204dcebc3bdc-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_975(CardRegistry& r) {
    r.registerCard(975, std::make_unique<EntrenchedBoar>());
}

}  // namespace riftbound
