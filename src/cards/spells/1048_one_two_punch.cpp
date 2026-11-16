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

class OneTwoPunch : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Buff] a friendly unit and [Stun] an enemy unit."
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTargetPair() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 2}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !friendlyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [f, e] = pickTargetPair(ctx, "One-Two Punch: a friendly unit, then an enemy unit",
            friendlyUnits(ctx.state, ctx.controller),
            [&](GameObjectId) { return enemyUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (f != kInvalidId && ctx.state.objectExists(f)) ctx.executor.buffUnit(f);
        if (e != kInvalidId && ctx.state.objectExists(e)) ctx.executor.stunUnit(e);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1048;
        d.def_id = R"RB(rad-121-167)RB";
        d.name = R"RB(One-Two Punch)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-121/167)RB";
        d.collector_number = 121;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Order};
        d.energy_cost = 3;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
[Buff] a friendly unit and [Stun] an enemy unit. (Give the friendly unit a +1 [M] buff if it doesn't have one. The enemy unit doesn't deal combat damage this turn.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/1f246286c658d44dc4e7d51c158b2f822da39825-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1048(CardRegistry& r) {
    r.registerCard(1048, std::make_unique<OneTwoPunch>());
}

}  // namespace riftbound
