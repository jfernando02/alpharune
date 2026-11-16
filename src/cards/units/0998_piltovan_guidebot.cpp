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

class PiltovanGuidebot : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a card, you may have my base Might become equal to that
    // card's Energy cost this turn." Taken when it raises my Might.
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayACard; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto subj = triggerSubject(ctx);
        if (subj == kInvalidId || !ctx.state.objectExists(subj) || !ctx.state.objectExists(ctx.source)) return;
        const int cost = printedEnergy(ctx, ctx.state.getObject(subj));
        auto& me = ctx.state.getObject(ctx.source);
        if (!me.location.has_value()) return;
        if (me.card_counters["__gb_turn"] != ctx.state.turn.turn_number) {
            me.card_counters["__gb_turn"] = ctx.state.turn.turn_number;
            me.card_counters["__gb_delta"] = 0;
        }
        const int want = cost - me.base_might;
        const int had = me.card_counters["__gb_delta"];
        if (want <= had) return;
        me.card_counters["__gb_delta"] = want;
        ctx.executor.giveTemporaryMight(ctx.source, want - had);
        ctx.events.logTrace("PILTOVAN GUIDEBOT: base Might becomes " + std::to_string(cost) + " this turn");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 998;
        d.def_id = R"RB(rad-057-167)RB";
        d.name = R"RB(Piltovan Guidebot)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-057/167)RB";
        d.collector_number = 57;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Mind};
        d.energy_cost = 4;
        d.might = 3;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you play a card, you may have my base Might become equal to that card's Energy cost this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9558ced7593f5a26911eb6e1393aae3289842e20-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_998(CardRegistry& r) {
    r.registerCard(998, std::make_unique<PiltovanGuidebot>());
}

}  // namespace riftbound
