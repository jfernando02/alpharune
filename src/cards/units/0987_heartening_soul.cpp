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

class HearteningSoul : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, ready a friendly legend. Friendly legends have
    // "[E]: Give a unit +3 [M] this turn.""
    // APPROX: the granted legend ability is not modeled.
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto leg = ctx.state.player(ctx.controller).legend_zone;
        if (leg != kInvalidId && ctx.state.objectExists(leg)) ctx.executor.readyObject(leg);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 987;
        d.def_id = R"RB(rad-040-167)RB";
        d.name = R"RB(Heartening Soul)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-040/167)RB";
        d.collector_number = 40;
        d.artist = R"RB(Dao Trong Le)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 7;
        d.power_cost = 1;
        d.might = 7;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play me, ready a friendly legend.
Friendly legends have "[E]: Give a unit +3 [M] this turn.")RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0a5f9ab3216462356f6ddf8f172284dca6be21e9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_987(CardRegistry& r) {
    r.registerCard(987, std::make_unique<HearteningSoul>());
}

}  // namespace riftbound
