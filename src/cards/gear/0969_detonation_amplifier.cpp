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

class DetonationAmplifier : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When you play this, you may kill another friendly unit or gear to
    // draw 1. Friendly [Deathknell] effects here trigger an additional time."
    int extraDeathknellTriggersHere() const override { return 1; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayThis; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    static std::vector<GameObjectId> fodder(const GameState& s, PlayerId p, GameObjectId self) {
        std::vector<GameObjectId> out;
        for (const auto& [id, o] : s.objects)
            if (id != self && o.controller == p && o.location.has_value() && (o.isUnit() || o.isGear()))
                out.push_back(id);
        return out;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int go = confirmOptional(ctx, "Detonation Amplifier: kill a friendly unit or gear to draw 1",
            [&] { return costAlreadyPaid(ctx) || !fodder(ctx.state, ctx.controller, ctx.source).empty(); });
        if (go == -1) return;
        GameObjectId victim = kInvalidId;
        if (go == 1) {
            victim = pickTarget(ctx, "Detonation Amplifier: kill", costChoices(ctx, fodder(ctx.state, ctx.controller, ctx.source)));
            if (ctx.executor.hasPendingChoice()) return;
            if (costDueNow(ctx) && victim != kInvalidId) ctx.executor.killObject(victim);
        }
        if (finalizingChoices(ctx)) return;
        if (go == 1 && (victim != kInvalidId || costAlreadyPaid(ctx))) ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 969;
        d.def_id = R"RB(rad-017-167)RB";
        d.name = R"RB(Detonation Amplifier)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-017/167)RB";
        d.collector_number = 17;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Fury};  // the gallery says Chaos (a mislabel): its collector number is in the Fury block, as riftdecks lists it
        d.energy_cost = 4;
        d.power_cost = 1;
        d.rarity = Rarity::Rare;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When you play this, you may kill another friendly unit or gear to draw 1.
Friendly [Deathknell] effects here trigger an additional time.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0cd07e77e69a53b78486b07dd7610183c953d581-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_969(CardRegistry& r) {
    r.registerCard(969, std::make_unique<DetonationAmplifier>());
}

}  // namespace riftbound
