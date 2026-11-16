#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "When you play me, deal 1 to each of up to 2 units."
// The two units are chosen when the trigger is finalized (CR 383.3).
// The player chooses no unit, one or two; any units.
class BilgewaterTracker : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        // "up to 2 units": any units; the player chooses none, one or two.
        auto units = chooseableUnits(ctx.state, me, [](const GameObject&) { return true; });
        int how_many = 0;
        if (!units.empty()) {
            const int modes = units.size() >= 2 ? 3 : 2;
            std::vector<std::string> labels = {"no unit", "1 unit", "2 units"};
            labels.resize(modes);
            how_many = pickMode(ctx, "Bilgewater Tracker: deal 1 to how many units?", modes, labels);
            if (how_many == -1) return;   // waiting for the answer
        }
        GameObjectId a = kInvalidId, b = kInvalidId;
        if (how_many == 1) {
            a = pickTarget(ctx, "Bilgewater Tracker: deal 1 to a unit", units);
        } else if (how_many == 2) {
            auto second = [&](GameObjectId first) {
                std::vector<GameObjectId> out;
                for (auto id : units) if (id != first) out.push_back(id);
                return out;
            };
            std::tie(a, b) = pickTargetPair(ctx, "Bilgewater Tracker: deal 1 to each of 2 units",
                                            units, second);
        }
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
