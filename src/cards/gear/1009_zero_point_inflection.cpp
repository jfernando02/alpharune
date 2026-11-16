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

class ZeroPointInflection : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a card, draw 2. When you draw 1 or more cards with 9 or more
    // other cards in your hand, discard your hand, kill this, and choose an
    // opponent. They score 1 point."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayACard, TriggerType::WhenYouDrawACard};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source) || !ctx.state.getObject(ctx.source).location) return;
        auto& ps = ctx.state.player(ctx.controller);
        if (ctx.firing_trigger == TriggerType::WhenYouPlayACard) { ctx.executor.drawCards(ctx.controller, 2); return; }
        if (ps.hand.size() < 10) return;
        const auto hand = ps.hand;
        for (auto c : hand) ctx.executor.applyDiscard(ctx.controller, c);
        ctx.executor.killObject(ctx.source);
        ctx.executor.scorePoints(opponent(ctx.controller), 1);
        ctx.events.logTrace("ZERO POINT INFLECTION: hand discarded, opponent scores 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1009;
        d.def_id = R"RB(rad-069-167)RB";
        d.name = R"RB(Zero Point Inflection)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-069/167)RB";
        d.collector_number = 69;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Mind};
        d.energy_cost = 9;
        d.power_cost = 2;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(When you play a card, draw 2.
When you draw 1 or more cards with 9 or more other cards in your hand, discard your hand, kill this, and choose an opponent. They score 1 point.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5e0646270d27f9fa78f4c3a637695e5cdeb8ee67-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1009(CardRegistry& r) {
    r.registerCard(1009, std::make_unique<ZeroPointInflection>());
}

}  // namespace riftbound
