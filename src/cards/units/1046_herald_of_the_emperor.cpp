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

class HeraldOfTheEmperor : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, choose a friendly unit. Its base Might becomes 5 this turn."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Herald of the Emperor: base Might becomes 5", friendlyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t == kInvalidId || !ctx.state.objectExists(t)) return;
        const int delta = 5 - ctx.state.getObject(t).base_might;
        if (delta != 0) ctx.executor.giveTemporaryMight(t, delta);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1046;
        d.def_id = R"RB(rad-118-167)RB";
        d.name = R"RB(Herald of the Emperor)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-118/167)RB";
        d.collector_number = 118;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 5;
        d.might = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play me, choose a friendly unit. Its base Might becomes 5 this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/a168856002994f7ac67150d991e65152e6434ab6-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1046(CardRegistry& r) {
    r.registerCard(1046, std::make_unique<HeraldOfTheEmperor>());
}

}  // namespace riftbound
