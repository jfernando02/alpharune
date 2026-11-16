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

class ClocktowerGuardian : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me or when I hold, [Predict 3]. [Deathknell][>] Banish the
    // top card of your Main Deck. You may play it, ignoring its Energy cost."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayMe, TriggerType::WhenIHold, TriggerType::WhenIDie};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger != TriggerType::WhenIDie) { ctx.executor.predict(ctx.controller, 3); return; }
        auto& ps = ctx.state.player(ctx.controller);
        if (ps.main_deck.empty()) return;
        const auto top = ps.main_deck.back();
        ps.main_deck.pop_back();
        auto& c = ctx.state.getObject(top);
        c.zone = ZoneType::Banishment;
        c.location = std::nullopt;
        ps.banishment.push_back(top);
        ctx.events.emit(CardRevealedEvent{top, c.card_def_id, c.owner, true, ctx.controller, ZoneType::MainDeck});
        EffectPlayCost cost;
        cost.ignore_energy = true;
        if (ctx.executor.canPlayPayingCost(ctx.controller, top, cost)) {
            ps.banishment.erase(std::find(ps.banishment.begin(), ps.banishment.end(), top));
            if (!ctx.executor.playPayingCost(ctx.controller, top, std::nullopt, cost)) {
                ctx.state.getObject(top).zone = ZoneType::Banishment;
                ps.banishment.push_back(top);
            }
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1005;
        d.def_id = R"RB(rad-067-167)RB";
        d.name = R"RB(Clocktower Guardian)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-067/167)RB";
        d.collector_number = 67;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};
        d.energy_cost = 8;
        d.power_cost = 2;
        d.might = 8;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Deathknell);
        d.ability_text = R"RB(When you play me or when I hold, [Predict 3].
[Deathknell][>] Banish the top card of your Main Deck. You may play it, ignoring its Energy cost.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b846d604eee450a4ab8f457f2738b0596e7b361e-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1005(CardRegistry& r) {
    r.registerCard(1005, std::make_unique<ClocktowerGuardian>());
}

}  // namespace riftbound
