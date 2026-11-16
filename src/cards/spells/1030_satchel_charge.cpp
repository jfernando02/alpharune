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

class SatchelCharge : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Move a friendly unit, then play a Bomb gear token to a battlefield."
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        const int bf = pickBattlefield(*this, ctx, "Satchel Charge: Bomb to a battlefield");
        if (bf == -1) return;
        const auto u = pickTarget(ctx, "Satchel Charge: move a friendly unit", friendlyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (u != kInvalidId && ctx.state.objectExists(u) && ctx.state.getObject(u).location) {
            if (ri.resume_point < 20) {
                std::vector<Intent> opts;
                for (int d : moveDests(ctx.state, ctx.state.getObject(u)))
                    opts.push_back(valChoice(ctx.controller, d, destLabel(d)));
                if (!opts.empty()) {
                    ctx.executor.requestChoice(ctx.controller, std::move(opts), "Satchel Charge: move where");
                    ri.resume_point = 20;
                    return;
                }
            } else if (ri.resume_point == 20) {
                moveTo(ctx, u, takenValue(ctx, 0));
                ri.resume_point = 21;
            }
        }
        if (bf >= 0) createBomb(ctx, static_cast<BattlefieldId>(bf));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1030;
        d.def_id = R"RB(rad-096-167)RB";
        d.name = R"RB(Satchel Charge)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-096/167)RB";
        d.collector_number = 96;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Chaos};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Move a friendly unit, then play a Bomb gear token to a battlefield. (It has [Deploy]. When it dies, deal 2 to an enemy there and kill your other Bombs there.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b07c26438f600b87c92e4ed778a1c8b073b47f5c-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1030(CardRegistry& r) {
    r.registerCard(1030, std::make_unique<SatchelCharge>());
}

}  // namespace riftbound
