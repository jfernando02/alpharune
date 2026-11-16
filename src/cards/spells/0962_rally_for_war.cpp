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

class RallyForWar : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Choose an occupied enemy battlefield. Play three 1 [M] Recruit unit tokens there."
    static bool occupiedEnemy(const GameState& s, PlayerId me, BattlefieldId b) {
        for (const auto& bf : s.battlefields)
            if (bf.id == b) {
                if (!bf.controller.has_value() || *bf.controller == me) return false;
                for (const auto& [id, o] : s.objects)
                    if (o.isUnit() && o.controller != me && o.battlefieldId() == b) return true;
            }
        return false;
    }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override {
        for (const auto& bf : s.battlefields) if (occupiedEnemy(s, p, bf.id)) return true;
        return false;
    }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int bf = pickBattlefield(*this, ctx, "Rally for War: an occupied enemy battlefield",
            [&](BattlefieldId b) { return occupiedEnemy(ctx.state, ctx.controller, b); });
        if (bf == -1) return;
        if (finalizingChoices(ctx)) return;
        if (bf < 0) return;
        for (int i = 0; i < 3; ++i)
            createRecruit(ctx, LocationId{BattlefieldLocation{static_cast<BattlefieldId>(bf)}});
        ctx.events.logTrace("RALLY FOR WAR: three Recruits to battlefield #" + std::to_string(bf));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 962;
        d.def_id = R"RB(rad-007-167)RB";
        d.name = R"RB(Rally for War)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-007/167)RB";
        d.collector_number = 7;
        d.artist = R"RB(Wild Blue Studios)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(Choose an occupied enemy battlefield. Play three 1 [M] Recruit unit tokens there.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/09c0fc35d8f4b72f58a95d1745b99c4133d02513-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_962(CardRegistry& r) {
    r.registerCard(962, std::make_unique<RallyForWar>());
}

}  // namespace riftbound
