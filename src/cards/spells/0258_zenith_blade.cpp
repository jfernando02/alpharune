#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class ZenithBlade : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    bool isActionAbility() const override { return true; }

    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true,
                                  .must_be_enemy = true,
                                  .must_be_at_battlefield = true};
    }
    bool needsPlayTimeTarget() const override { return true; }

    void onResolve(CardContext& ctx,
                   const std::vector<GameObjectId>& /*targets*/) override {
        PlayerId opp = opponent(ctx.controller);
        std::vector<GameObjectId> enemy_at_bf;
        for (auto& [id, obj] : ctx.state.objects) {
            if (!obj.isUnit() || obj.controller != opp) continue;
            if (!obj.isAtBattlefield()) continue;
            if (obj.untargetable_by_enemy) continue;
            enemy_at_bf.push_back(id);
        }
        std::sort(enemy_at_bf.begin(), enemy_at_bf.end());
        if (enemy_at_bf.empty()) return;   // no legal target: fizzle
        // "You may move a friendly unit to that enemy unit's battlefield." Asked
        // first (confirmOptional's resume points come before pickTarget's),
        // then the enemy to stun, then the friendly unit to move.
        auto movable = [&](std::optional<LocationId> dest) {
            std::vector<GameObjectId> out;
            for (auto& [id, obj] : ctx.state.objects) {
                if (!obj.isUnit() || obj.controller != ctx.controller) continue;
                if (!obj.location.has_value()) continue;
                if (dest && obj.location == dest) continue;  // already there
                out.push_back(id);
            }
            return out;
        };
        int conf = confirmOptional(ctx,
            "Zenith Blade: move a friendly unit to that battlefield?",
            [&] { return !movable(std::nullopt).empty(); });
        if (conf == -1) return;  // waiting on agent
        GameObjectId stunned = pickTarget(ctx, "Zenith Blade: stun enemy unit",
                                          enemy_at_bf);
        if (stunned == kInvalidId || !ctx.state.objectExists(stunned)) return;
        std::optional<LocationId> dest = ctx.state.getObject(stunned).location;
        ctx.executor.stunUnitBy(stunned, ctx.source);   // nothing after this suspends

        if (conf == 0) return;   // declined / nothing to move
        if (!dest.has_value() ||
            !std::holds_alternative<BattlefieldLocation>(*dest)) return;
        const BattlefieldId dest_bf = std::get<BattlefieldLocation>(*dest).id;
        const auto units = movable(dest);
        if (units.empty()) return;
        const GameObjectId mover = ctx.executor.chooseObjectNow(ctx.controller, units);
        if (mover == kInvalidId) return;
        ctx.executor.moveToBattlefield(mover, dest_bf);
        ctx.events.logTrace("ZENITH BLADE: stunned enemy + moved a friendly "
                            "unit to that battlefield");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 258;
        d.def_id = R"RB(ogn-262-298)RB";
        d.name = R"RB(Zenith Blade)RB";
        d.set_code = R"RB(OGN)RB";
        d.set_name = R"RB(Origins)RB";
        d.public_code = R"RB(OGN-262/298)RB";
        d.collector_number = 262;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.super_type = SuperType::Signature;
        d.domains = {Domain::Calm, Domain::Order};
        d.tags = {R"RB(Leona)RB"};
        d.energy_cost = 3;
        d.power_cost = 2;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Stun an enemy unit at a battlefield. You may move a friendly unit to that enemy unit's battlefield. (A stunned unit doesn't deal combat damage this turn.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/a572f0bc3ed1bd47c5759d36f5b951b15338e57e-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_258(CardRegistry& r) {
    r.registerCard(258, std::make_unique<ZenithBlade>());
}

} // namespace riftbound
