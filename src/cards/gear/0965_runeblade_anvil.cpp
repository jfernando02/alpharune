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

class RunebladeAnvil : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play this or when this leaves the board, banish the top card of
    // your Main Deck. Until your next Ending Phase, you may play that card.
    // [2][P], [E]: Kill this."
    // APPROX: the banished card may be played (paying its cost) as the ability
    // resolves, not later; "leaves the board" counts only its death.
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayThis, TriggerType::WhenIDie};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        if (ps.main_deck.empty()) return;
        const auto top = ps.main_deck.back();
        ps.main_deck.pop_back();
        auto& c = ctx.state.getObject(top);
        c.zone = ZoneType::Banishment;
        c.location = std::nullopt;
        ps.banishment.push_back(top);
        ctx.events.emit(CardRevealedEvent{top, c.card_def_id, c.owner, true, ctx.controller, ZoneType::MainDeck});
        ctx.events.logTrace("RUNEBLADE ANVIL: banished " + c.name);
        if (ctx.executor.canPlayPayingCost(ctx.controller, top)) {
            ps.banishment.erase(std::find(ps.banishment.begin(), ps.banishment.end(), top));
            if (!ctx.executor.playPayingCost(ctx.controller, top)) {
                auto& back = ctx.state.getObject(top);
                back.zone = ZoneType::Banishment;
                ps.banishment.push_back(top);
            }
        }
    }
    bool hasActivatedAbility() const override { return true; }
    ActivationCost getActivationCost() const override {
        ActivationCost c; c.energy = 2; c.power = 1; c.power_domain = Domain::Chaos; c.exhaust = true;
        return c;
    }
    void onActivate(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.executor.killObject(ctx.source);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 965;
        d.def_id = R"RB(rad-012-167)RB";
        d.name = R"RB(Runeblade Anvil)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-012/167)RB";
        d.collector_number = 12;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Chaos};
        d.energy_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you play this or when this leaves the board, banish the top card of your Main Deck. Until your next Ending Phase, you may play that card. (You still pay its costs.)
[2][P], [E]: Kill this.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/11e2ddea70ec4dd8e562681a1ab1dfbc2b2dca0f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_965(CardRegistry& r) {
    r.registerCard(965, std::make_unique<RunebladeAnvil>());
}

}  // namespace riftbound
