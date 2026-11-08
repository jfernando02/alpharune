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

// "[E]: [Buff] a unit. If it's already buffed, give it +2 [M] this turn
// instead."
class IridescentAvian : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    std::vector<ActivatedAbility> activatedAbilities() const override {
        return {{
            .cost = {.exhaust = true},
            .targets = TargetRequirements{.count = 1, .must_be_unit = true},
            .is_action = false, .is_reaction = false,
            .needs_activation_time_target = true,
        }};
    }
    std::vector<GameObjectId> enumerateLegalTargets(const GameState& state, PlayerId controller,
                                                    int /*ability_index*/) const override {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects) {
            if (!obj.isUnit() || !obj.location.has_value()) continue;
            if (obj.controller != controller && obj.untargetable_by_enemy) continue;
            out.push_back(id);
        }
        return out;
    }
    void onActivate(CardContext& ctx, int /*ability_index*/,
                    const std::vector<GameObjectId>& targets) override {
        GameObjectId unit = targets.empty()
            ? pickTarget(ctx, "Iridescent Avian: buff a unit",
                         enumerateLegalTargets(ctx.state, ctx.controller, 0))
            : targets[0];
        if (unit == kInvalidId || !ctx.state.objectExists(unit)) return;
        if (ctx.state.getObject(unit).buff_count > 0) {
            ctx.executor.giveTemporaryMight(unit, 2);
            ctx.events.logTrace("IRIDESCENT AVIAN: already buffed -> +2 [M] this turn");
        } else {
            ctx.executor.buffUnit(unit);
            ctx.events.logTrace("IRIDESCENT AVIAN: buff");
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 979;
        d.def_id = R"RB(rad-027-167)RB";
        d.name = R"RB(Iridescent Avian)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-027/167)RB";
        d.collector_number = 27;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB([E]: [Buff] a unit. If it's already buffed, give it +2 [M] this turn instead. (To buff a unit, give it a +1 [M] buff if it doesn't have one.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/73fde9189b222313dcf8c2abe79b38082c76961e-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_979(CardRegistry& r) {
    r.registerCard(979, std::make_unique<IridescentAvian>());
}

} // namespace riftbound
