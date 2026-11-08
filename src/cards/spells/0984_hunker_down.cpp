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

// "Ready a friendly unit. It can't move this turn. Draw 1."
class HunkerDown : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    static std::vector<GameObjectId> friendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value())
                out.push_back(id);
        return out;
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true, .must_be_friendly = true};
    }
    std::vector<GameObjectId> enumerateLegalTargets(const GameState& state,
                                                    PlayerId controller) const override {
        return friendlyUnits(state, controller);
    }
    bool needsPlayTimeTarget() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId unit = pickTarget(ctx, "Hunker Down: ready a friendly unit",
                                             friendlyUnits(ctx.state, ctx.controller));
        if (unit == kInvalidId || !ctx.state.objectExists(unit)) return;
        ctx.executor.readyObject(unit);
        ctx.state.getObject(unit).cant_move_on_turn = ctx.state.turn.turn_number;
        ctx.executor.drawCards(ctx.controller, 1);
        ctx.events.logTrace("HUNKER DOWN: readied a friendly unit (can't move this turn), draw 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 984;
        d.def_id = R"RB(rad-034-167)RB";
        d.name = R"RB(Hunker Down)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-034/167)RB";
        d.collector_number = 34;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Ready a friendly unit. It can't move this turn. Draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/111c518590b17919e5b851ef6c713c7eb7504ae2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_984(CardRegistry& r) {
    r.registerCard(984, std::make_unique<HunkerDown>());
}

} // namespace riftbound
