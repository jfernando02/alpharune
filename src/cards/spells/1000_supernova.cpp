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

class Supernova : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Deal 10 to a unit. Draw 2."
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTarget() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 1}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !anyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Supernova: deal 10", anyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t) && ctx.state.getObject(t).location) dealAndKill(ctx, t, 10);
        ctx.executor.drawCards(ctx.controller, 2);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1000;
        d.def_id = R"RB(rad-059-167)RB";
        d.name = R"RB(Supernova)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-059/167)RB";
        d.collector_number = 59;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind};  // the gallery says Order (a mislabel): its collector number is in the Mind block, as riftdecks lists it
        d.energy_cost = 9;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(Deal 10 to a unit. Draw 2.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/bea8e402f99b0afb5af1b19c0ee8ea34052e736f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1000(CardRegistry& r) {
    r.registerCard(1000, std::make_unique<Supernova>());
}

}  // namespace riftbound
