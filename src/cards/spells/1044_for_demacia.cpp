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

class ForDemacia : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Choose a battlefield. Give friendly units there [Shield 2] this turn."
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int bf = pickBattlefield(*this, ctx, "For Demacia!: a battlefield", [&](BattlefieldId b) {
            return !friendlyUnitsAt(ctx.state, ctx.controller, b).empty(); });
        if (bf < 0) return;
        for (auto u : friendlyUnitsAt(ctx.state, ctx.controller, static_cast<BattlefieldId>(bf)))
            ctx.executor.giveTemporaryKeyword(u, Keyword::Shield, 2);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1044;
        d.def_id = R"RB(rad-117-167)RB";
        d.name = R"RB(For Demacia!)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-117/167)RB";
        d.collector_number = 117;
        d.artist = R"RB(Original Force)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Order};
        d.energy_cost = 3;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Choose a battlefield. Give friendly units there [Shield 2] this turn. (+2 [M] while they're defenders.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5d119653fcba423c150815de3a412922c4e481e8-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1044(CardRegistry& r) {
    r.registerCard(1044, std::make_unique<ForDemacia>());
}

}  // namespace riftbound
