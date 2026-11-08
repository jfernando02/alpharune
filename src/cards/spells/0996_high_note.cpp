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

// "Give a unit -[M] this turn equal to the number of exhausted units you
// control."
class HighNote : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    static std::vector<GameObjectId> targetableUnits(const GameState& state, PlayerId controller) {
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
        return targetableUnits(state, controller);
    }
    bool needsPlayTimeTarget() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId unit = pickTarget(ctx, "High Note: give a unit -M",
                                             targetableUnits(ctx.state, ctx.controller));
        if (unit == kInvalidId || !ctx.state.objectExists(unit)) return;
        int exhausted = 0;
        for (const auto& [id, obj] : ctx.state.objects)
            if (obj.isUnit() && obj.controller == ctx.controller && obj.location.has_value() &&
                obj.is_exhausted)
                ++exhausted;
        if (exhausted == 0) return;
        ctx.executor.giveTemporaryMight(unit, -exhausted);
        ctx.events.logTrace("HIGH NOTE: -" + std::to_string(exhausted) + " [M] this turn");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 996;
        d.def_id = R"RB(rad-048-167)RB";
        d.name = R"RB(High Note)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-048/167)RB";
        d.collector_number = 48;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind};
        d.energy_cost = 3;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Give a unit –[M] this turn equal to the number of exhausted units you control.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/afba3669ea1063dafb0e4c2c766694e00540eb13-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_996(CardRegistry& r) {
    r.registerCard(996, std::make_unique<HighNote>());
}

} // namespace riftbound
