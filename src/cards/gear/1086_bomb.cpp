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

// Bomb gear token (CR 187.12): "[Deploy]. [Deathknell][>] Deal 2 to an enemy
// unit here and kill all other Bombs you control here."
// Ziggs, Short-Fused (973): "Abilities of friendly gear deal 1 Bonus Damage."
class BombToken : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIDie; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        std::optional<LocationId> here;
        if (ctx.state.objectExists(ctx.source)) {
            const auto& me = ctx.state.getObject(ctx.source);
            here = me.location.has_value() ? me.location
                 : (me.last_known.valid ? me.last_known.location : me.last_location);
        }
        std::optional<BattlefieldId> bf;
        if (here && std::holds_alternative<BattlefieldLocation>(*here))
            bf = std::get<BattlefieldLocation>(*here).id;
        const auto t = pickTarget(ctx, "Bomb: deal 2 to an enemy unit here",
                                  enemyUnitsAt(ctx.state, ctx.controller, bf));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        int bonus = 0;
        for (const auto& [id, obj] : ctx.state.objects)
            if (obj.card_def_id == 973 && obj.controller == ctx.controller && obj.location.has_value())
                ++bonus;   // Ziggs, Short-Fused
        if (t != kInvalidId && ctx.state.objectExists(t) &&
            ctx.state.getObject(t).battlefieldId() == bf)
            dealAndKill(ctx, t, 2 + bonus);
        if (!bf) return;
        std::vector<GameObjectId> others;
        for (const auto& [id, obj] : ctx.state.objects)
            if (id != ctx.source && obj.card_def_id == kBombTokenCardDefId &&
                obj.controller == ctx.controller && obj.battlefieldId() == bf)
                others.push_back(id);
        for (auto id : others) ctx.executor.killObject(id);
        ctx.events.logTrace("BOMB: deal " + std::to_string(2 + bonus) + ", killed " +
                            std::to_string(others.size()) + " other Bomb(s) here");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = kBombTokenCardDefId;
        d.def_id = R"RB(rad-t-bomb)RB";
        d.name = R"RB(Bomb)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-T)RB";
        d.card_type = CardType::Gear;
        d.super_type = SuperType::Token;
        d.tags = {R"RB(Bomb)RB"};
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Deathknell);
        d.deploy = true;
        d.ability_text = R"RB([Deploy]
[Deathknell][>] Deal 2 to an enemy unit here and kill all other Bombs you control here.)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1086(CardRegistry& r) {
    r.registerCard(1086, std::make_unique<BombToken>());
}

}  // namespace riftbound
