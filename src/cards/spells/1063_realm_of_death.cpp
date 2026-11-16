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

class RealmOfDeath : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Give an enemy unit -2 [M] this turn and a friendly unit +2 [M] this turn.
    // They deal damage equal to their Mights to each other."
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTargetPair() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 2}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override {
        return !friendlyUnits(s, p).empty() && !enemyUnits(s, p).empty();
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [e, f] = pickTargetPair(ctx, "Realm of Death: an enemy unit, then a friendly unit",
            enemyUnits(ctx.state, ctx.controller),
            [&](GameObjectId) { return friendlyUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        const bool he = e != kInvalidId && ctx.state.objectExists(e) && ctx.state.getObject(e).location;
        const bool hf = f != kInvalidId && ctx.state.objectExists(f) && ctx.state.getObject(f).location;
        if (he) ctx.executor.giveTemporaryMight(e, -2);
        if (hf) ctx.executor.giveTemporaryMight(f, 2);
        if (!he || !hf) return;
        const int me = std::max(0, ctx.state.getObject(e).current_might);
        const int mf = std::max(0, ctx.state.getObject(f).current_might);
        ctx.executor.dealDamage(f, me, e);
        ctx.executor.dealDamage(e, mf, f);
        killLethal(ctx, {e, f});
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1063;
        d.def_id = R"RB(rad-144-167)RB";
        d.name = R"RB(Realm of Death)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-144/167)RB";
        d.collector_number = 144;
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury, Domain::Order};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Give an enemy unit -2 [M] this turn and a friendly unit +2 [M] this turn. They deal damage equal to their Mights to each other.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/fd3b4c6939b6a80b33956da7e3a9102eba9c32a8-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1063(CardRegistry& r) {
    r.registerCard(1063, std::make_unique<RealmOfDeath>());
}

}  // namespace riftbound
