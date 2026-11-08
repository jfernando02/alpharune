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

// "When you play this, give a friendly unit +3 [M] this turn.
//  Exhaust a friendly [Mighty] unit, [E]: Draw 1."
// The ability's extra cost (exhausting a ready Mighty unit) gates activation
// and is chosen and paid when the ability is finalized.
class InvigoratingBloom : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger | kFinalizeActivate; }

    static std::vector<GameObjectId> friendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value())
                out.push_back(id);
        return out;
    }
    static std::vector<GameObjectId> readyMightyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value() &&
                !obj.is_exhausted && obj.current_might >= 5)
                out.push_back(id);
        return out;
    }

    TriggerType triggerType() const override { return TriggerType::WhenYouPlayThis; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId unit = pickTarget(ctx, "Invigorating Bloom: give a friendly unit +3 [M]",
                                             friendlyUnits(ctx.state, ctx.controller));
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (unit == kInvalidId || !ctx.state.objectExists(unit)) return;
        ctx.executor.giveTemporaryMight(unit, 3);
        ctx.events.logTrace("INVIGORATING BLOOM: +3 [M] this turn");
    }

    std::vector<ActivatedAbility> activatedAbilities() const override {
        return {{.cost = {.exhaust = true}}};
    }
    bool canActivateAbilityAt(const GameState& state, PlayerId controller, GameObjectId source,
                              int ability_index) const override {
        return Card::canActivateAbilityAt(state, controller, source, ability_index) &&
               !readyMightyUnits(state, controller).empty();
    }
    void onActivate(CardContext& ctx, int /*ability_index*/,
                    const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId paid = pickTarget(ctx, "Invigorating Bloom: exhaust a friendly Mighty unit",
                                             costChoices(ctx, readyMightyUnits(ctx.state, ctx.controller)));
        if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
        if (paid != kInvalidId && ctx.state.objectExists(paid) && costDueNow(ctx))
            ctx.executor.exhaustObject(paid);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (paid == kInvalidId && !costAlreadyPaid(ctx)) return;  // the cost was never paid
        ctx.executor.drawCards(ctx.controller, 1);
        ctx.events.logTrace("INVIGORATING BLOOM: exhausted a Mighty unit -> draw 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 985;
        d.def_id = R"RB(rad-035-167)RB";
        d.name = R"RB(Invigorating Bloom)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-035/167)RB";
        d.collector_number = 35;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Calm};
        d.energy_cost = 3;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you play this, give a friendly unit +3 [M] this turn.
Exhaust a friendly [Mighty] unit, [E]: Draw 1. (A unit is Mighty while it has 5+ [M].))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/66d4436ef5d7a7a81c60ee9b1b3ebae06162b7d2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_985(CardRegistry& r) {
    r.registerCard(985, std::make_unique<InvigoratingBloom>());
}

} // namespace riftbound
