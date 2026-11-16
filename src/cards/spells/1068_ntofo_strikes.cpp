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

class NtofoStrikes : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Give a friendly unit [Shield 2] this turn, then deal damage equal to its
    // [Shield] to an enemy unit."
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTargetPair() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 2}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !friendlyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [f, e] = pickTargetPair(ctx, "Ntofo Strikes: a friendly unit, then an enemy unit",
            friendlyUnits(ctx.state, ctx.controller),
            [&](GameObjectId) { return enemyUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (f == kInvalidId || !ctx.state.objectExists(f) || !ctx.state.getObject(f).location) return;
        const int s = totalShield(ctx.state, f) + 2;
        ctx.executor.giveTemporaryKeyword(f, Keyword::Shield, 2);
        if (e != kInvalidId && ctx.state.objectExists(e) && ctx.state.getObject(e).location) dealAndKill(ctx, e, s);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1068;
        d.def_id = R"RB(rad-148-167)RB";
        d.name = R"RB(Ntofo Strikes)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-148/167)RB";
        d.collector_number = 148;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm, Domain::Body};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Give a friendly unit [Shield 2] this turn, then deal damage equal to its [Shield] to an enemy unit. (A unit with Shield has +1 [M] for each instance of Shield while it's a defender.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ab5aa7d29bd49efe2c9da3b62a8065295c189991-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1068(CardRegistry& r) {
    r.registerCard(1068, std::make_unique<NtofoStrikes>());
}

}  // namespace riftbound
