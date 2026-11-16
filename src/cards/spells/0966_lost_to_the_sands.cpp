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

class LostToTheSands : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Choose one — Deal 4 to a unit at a battlefield. / Kill a gear."
    static std::vector<GameObjectId> bfUnits(const GameState& s, PlayerId p) {
        return chooseableUnits(s, p, [](const GameObject& o) { return o.isAtBattlefield(); });
    }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override {
        return !bfUnits(s, p).empty() || !boardGear(s, p).empty();
    }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        uint32_t legal = 0;
        if (!bfUnits(ctx.state, ctx.controller).empty()) legal |= 1u;
        if (!boardGear(ctx.state, ctx.controller).empty()) legal |= 2u;
        const int mode = pickMode(ctx, "Lost to the Sands", 2, {"deal 4 to a unit at a battlefield", "kill a gear"}, legal);
        if (mode == -1) return;
        if (mode < 0) { if (finalizingChoices(ctx)) return; return; }
        const auto t = pickTarget(ctx, "Lost to the Sands: target",
            mode == 0 ? bfUnits(ctx.state, ctx.controller) : boardGear(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t == kInvalidId || !ctx.state.objectExists(t) || !ctx.state.getObject(t).location) return;
        if (mode == 0) dealAndKill(ctx, t, 4);
        else ctx.executor.killObject(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 966;
        d.def_id = R"RB(rad-013-167)RB";
        d.name = R"RB(Lost to the Sands)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-013/167)RB";
        d.collector_number = 13;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Choose one —Deal 4 to a unit at a battlefield.Kill a gear.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/92071a0f4d495f5f7acdf78d060133b98d1cebae-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_966(CardRegistry& r) {
    r.registerCard(966, std::make_unique<LostToTheSands>());
}

}  // namespace riftbound
