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

class ShimmeringSwimmer : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "You may kill a friendly unit or gear as an additional cost to play me.
    // When you play me, if you paid the additional cost, deal 2 to a unit."
    OptionalAdditionalCost optionalAdditionalCost() const override {
        OptionalAdditionalCost c;
        c.valid = true;
        c.kill_friendly_unit = true;
        c.kill_friendly_gear = true;
        c.paid_flag = "__swimmer_paid";
        return c;
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId, TriggerType, GameObjectId) const override {
        return s.objectExists(self) && s.getObject(self).card_counters.count("__swimmer_paid") > 0;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Shimmering Swimmer: deal 2 to a unit", anyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t) && ctx.state.getObject(t).location) dealAndKill(ctx, t, 2);
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 967;
        d.def_id = R"RB(rad-014-167)RB";
        d.name = R"RB(Shimmering Swimmer)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-014/167)RB";
        d.collector_number = 14;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(You may kill a friendly unit or gear as an additional cost to play me.
When you play me, if you paid the additional cost, deal 2 to a unit.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/02ef2a5c93cf60048bb932a709d3a5b17b93bb1d-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_967(CardRegistry& r) {
    r.registerCard(967, std::make_unique<ShimmeringSwimmer>());
}

}  // namespace riftbound
