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

class DocksideLockUp : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] When you play this, banish a unit here. When you hold here, kill
    // this to play the banished card, ignoring its cost."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayThis, TriggerType::WhenYouHoldHere};
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger == TriggerType::WhenYouHoldHere) {
            if (finalizingChoices(ctx)) return;
            if (!ctx.state.objectExists(ctx.source)) return;
            auto& me = ctx.state.getObject(ctx.source);
            const auto it = me.card_counters.find("__locked");
            if (it == me.card_counters.end() || !me.location) return;
            const auto c = static_cast<GameObjectId>(it->second);
            me.card_counters.erase(it);
            ctx.executor.killObject(ctx.source);
            if (!ctx.state.objectExists(c) || ctx.state.getObject(c).zone != ZoneType::Banishment) return;
            pullFromZoneLists(ctx.state, c);
            ctx.executor.playIgnoringCost(ctx.controller, c);
            return;
        }
        const auto t = pickTarget(ctx, "Dockside Lock-Up: banish a unit here",
                                  keepHere(ctx.state, ctx.source, anyUnits(ctx.state, ctx.controller)));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t == kInvalidId || !ctx.state.objectExists(t) || !ctx.state.getObject(t).location) return;
        const bool token = ctx.state.getObject(t).is_token;
        ctx.executor.banishObject(t);
        if (!token && ctx.state.objectExists(ctx.source))
            ctx.state.getObject(ctx.source).card_counters["__locked"] = static_cast<int>(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1056;
        d.def_id = R"RB(rad-136-167)RB";
        d.name = R"RB(Dockside Lock-Up)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-136/167)RB";
        d.collector_number = 136;
        d.card_type = CardType::Gear;
        d.domains = {Domain::Order};
        d.energy_cost = 6;
        d.power_cost = 2;
        d.rarity = Rarity::Epic;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
When you play this, banish a unit here.
When you hold here, kill this to play the banished card, ignoring its cost. (Tokens aren't cards.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/4f477ccc474cb16e4e9d681b388f0673f77fae5b-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1056(CardRegistry& r) {
    r.registerCard(1056, std::make_unique<DocksideLockUp>());
}

}  // namespace riftbound
