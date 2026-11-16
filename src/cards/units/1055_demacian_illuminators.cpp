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

class DemacianIlluminators : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When one or more friendly units die, [Buff] a friendly unit."
    // Triggers once for units that die at the same time (one death batch).
    bool triggerCondition(const GameState& state, GameObjectId /*source*/, PlayerId controller,
                          TriggerType /*trigger*/, GameObjectId subject) const override {
        return firstDeathInBatch(state, controller, subject);
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenAFriendlyUnitDies; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Demacian Illuminators: [Buff] a friendly unit", friendlyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t)) ctx.executor.buffUnit(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1055;
        d.def_id = R"RB(rad-131-167)RB";
        d.name = R"RB(Demacian Illuminators)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-131/167)RB";
        d.collector_number = 131;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.might = 3;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When one or more friendly units die, [Buff] a friendly unit. (Give it a +1 [M] buff if it doesn't have one.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/127c2700ee4e2557dfd463ed84c45f4600ad9723-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1055(CardRegistry& r) {
    r.registerCard(1055, std::make_unique<DemacianIlluminators>());
}

}  // namespace riftbound
