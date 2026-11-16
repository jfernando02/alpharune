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

class KSanteNtofosChosen : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When I attack or defend, double the [Shield] of a unit you control here
    // this turn."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIAttackOrDefend; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "K'Sante: double a unit's [Shield]",
                                  keepHere(ctx.state, ctx.source, friendlyUnits(ctx.state, ctx.controller)));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        const int s = totalShield(ctx.state, t);
        if (t != kInvalidId && s > 0) ctx.executor.giveTemporaryKeyword(t, Keyword::Shield, s);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 989;
        d.def_id = R"RB(rad-045-167)RB";
        d.name = R"RB(K'Sante, Dauntless)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-045/167)RB";
        d.collector_number = 45;
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Calm};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.might = 4;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Shield);
        d.shield_value = 1;
        d.ability_text = R"RB([Shield] (+1 [M] while I'm a defender.)
When I attack or defend, double the [Shield] of a unit you control here this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/f56e9f9db199f915e32ea264b7dab70202f97957-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_989(CardRegistry& r) {
    r.registerCard(989, std::make_unique<KSanteNtofosChosen>());
}

}  // namespace riftbound
