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

class Brawl : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Give a unit [Assault] this turn equal to its Might."
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTarget() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 1}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !anyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Brawl: a unit", anyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t == kInvalidId || !ctx.state.objectExists(t) || !ctx.state.getObject(t).location) return;
        const int m = ctx.state.getObject(t).current_might;
        if (m > 0) ctx.executor.giveTemporaryKeyword(t, Keyword::Assault, m);
        ctx.events.logTrace("BRAWL: Assault " + std::to_string(m));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 959;
        d.def_id = R"RB(rad-004-167)RB";
        d.name = R"RB(Brawl)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-004/167)RB";
        d.collector_number = 4;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Chaos};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Give a unit [Assault] this turn equal to its Might. (+1 [M] for each instance of Assault while it's an attacker.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/38fd13c7522bb47dcc5ce1596aeb6a638087ee09-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_959(CardRegistry& r) {
    r.registerCard(959, std::make_unique<Brawl>());
}

}  // namespace riftbound
