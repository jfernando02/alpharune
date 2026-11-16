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

class StalwartGoatherd : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Shield 2] When you play me, you may kill a gear with Energy cost [4] or less."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    static std::vector<GameObjectId> cheapGear(const CardContext& ctx) {
        std::vector<GameObjectId> out;
        for (auto g : boardGear(ctx.state, ctx.controller))
            if (printedEnergy(ctx, ctx.state.getObject(g)) <= 4) out.push_back(g);
        return out;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int go = confirmOptional(ctx, "Stalwart Goatherd: kill a gear costing [4] or less",
                                       [&] { return !cheapGear(ctx).empty(); });
        if (go == -1) return;
        if (go == 0) { if (finalizingChoices(ctx)) return; return; }
        const auto t = pickTarget(ctx, "Stalwart Goatherd: kill a gear", cheapGear(ctx));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t) && ctx.state.getObject(t).location) ctx.executor.killObject(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1021;
        d.def_id = R"RB(rad-083-167)RB";
        d.name = R"RB(Stalwart Goatherd)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-083/167)RB";
        d.collector_number = 83;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Body};
        d.energy_cost = 4;
        d.might = 2;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Shield);
        d.shield_value = 2;
        d.ability_text = R"RB([Shield 2] (+2 [M] while I'm a defender.)
When you play me, you may kill a gear with Energy cost [4] or less.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/bff85727b58017cbd3921fd73e9b363f5a0205e4-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1021(CardRegistry& r) {
    r.registerCard(1021, std::make_unique<StalwartGoatherd>());
}

}  // namespace riftbound
