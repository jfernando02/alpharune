#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "When you play me, deal 1 to each of up to 2 units."
// The two units are chosen when the trigger is finalized (CR 383.3).
// APPROX: the picks are enemy units — the target pickers have no "none" option,
// and dealing 1 to a unit of your own is next to never wanted.
class BilgewaterTracker : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        auto enemies = chooseableUnits(ctx.state, me,
                                       [&](const GameObject& o) { return o.controller != me; });
        auto second = [&](GameObjectId a) {
            std::vector<GameObjectId> out;
            for (auto id : enemies) if (id != a) out.push_back(id);
            return out;
        };
        auto [a, b] = pickTargetPair(ctx, "Bilgewater Tracker: deal 1 to each of up to 2 units",
                                     enemies, second);
        if (ctx.executor.hasPendingChoice()) return;   // waiting for a pick
        if (finalizingChoices(ctx)) return;            // choices end here (CR 383.3)
        for (auto t : {a, b})
            if (t != kInvalidId && ctx.state.objectExists(t) &&
                ctx.state.getObject(t).location.has_value())
                ctx.executor.dealDamage(t, 1, ctx.source);
        ctx.events.logTrace("BILGEWATER TRACKER: dealt 1 to up to 2 units");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 958;
        d.def_id = R"RB(rad-003-167)RB";
        d.name = R"RB(Bilgewater Tracker)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-003/167)RB";
        d.collector_number = 3;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};   // the gallery says Body (a preview mislabel): RAD-003 is in the Fury block and Fury/Mind Kai'Sa lists play it
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play me, deal 1 to each of up to 2 units.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b660759a6bcb9c7ca41ea969cf11463259e284c9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_958(CardRegistry& r) {
    r.registerCard(958, std::make_unique<BilgewaterTracker>());
}

} // namespace riftbound
