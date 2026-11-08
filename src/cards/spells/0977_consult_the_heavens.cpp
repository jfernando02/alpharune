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

// "Give a unit [Shield 2] this turn. [Predict]."
class ConsultTheHeavens : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    static std::vector<GameObjectId> shieldableUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects) {
            if (!obj.isUnit() || !obj.location.has_value()) continue;
            if (obj.controller != controller && obj.untargetable_by_enemy) continue;
            out.push_back(id);
        }
        return out;
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true};
    }
    std::vector<GameObjectId> enumerateLegalTargets(const GameState& state,
                                                    PlayerId controller) const override {
        return shieldableUnits(state, controller);
    }
    bool needsPlayTimeTarget() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId unit = pickTarget(ctx, "Consult the Heavens: give a unit Shield 2",
                                             shieldableUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;  // waiting for the target pick
        if (unit != kInvalidId && ctx.state.objectExists(unit))
            ctx.executor.giveTemporaryKeyword(unit, Keyword::Shield, 2);
        ctx.executor.predict(ctx.controller, 1);
        ctx.events.logTrace("CONSULT THE HEAVENS: Shield 2 + predict");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 977;
        d.def_id = R"RB(rad-025-167)RB";
        d.name = R"RB(Consult the Heavens)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-025/167)RB";
        d.collector_number = 25;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Reaction);
        d.keywords.set(Keyword::Predict);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Give a unit [Shield 2] this turn. (+2 [M] while it's a defender.)
[Predict]. (Look at the top card of your Main Deck. You may recycle it.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/120b20ee26d221cea31f510090cef037e74c023f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_977(CardRegistry& r) {
    r.registerCard(977, std::make_unique<ConsultTheHeavens>());
}

} // namespace riftbound
