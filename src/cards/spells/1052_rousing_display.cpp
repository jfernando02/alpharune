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

class RousingDisplay : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Show Off] a card with Energy cost [7] or more. Play two 1 [M] Recruit
    // unit tokens to your base. If you showed off, play four instead."
    // Shows off whenever it can (always better).
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        bool show = false;
        for (auto c : ctx.state.player(ctx.controller).hand)
            if (c != ctx.source && printedEnergy(ctx, ctx.state.getObject(c)) >= 7) show = true;
        for (const auto& [id, o] : ctx.state.objects)
            if (o.controller == ctx.controller && o.location.has_value() && !o.isRune() &&
                !o.is_token && printedEnergy(ctx, o) >= 7) show = true;
        const int n = show ? 4 : 2;
        for (int i = 0; i < n; ++i) createRecruitAtBase(ctx);
        ctx.events.logTrace("ROUSING DISPLAY: " + std::to_string(n) + " Recruits");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1052;
        d.def_id = R"RB(rad-127-167)RB";
        d.name = R"RB(Rousing Display)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-127/167)RB";
        d.collector_number = 127;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Order};
        d.energy_cost = 5;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB([Show Off] a card with Energy cost [7] or more. (As you play this, you may reveal such a card from hand or pick a friendly one.)
Play two 1 [M] Recruit unit tokens to your base. If you showed off, play four to your base instead.disa)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/18aa7cab082fd0e293260bcf480a1a76d12b64d9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1052(CardRegistry& r) {
    r.registerCard(1052, std::make_unique<RousingDisplay>());
}

}  // namespace riftbound
