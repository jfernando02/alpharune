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

class AsheFocused : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>&) override {
        // "When they hold, return it to their hand (even if I'm no longer on
        // the board)": a delayed ability scheduled below, carrying the card.
        if (ctx.firing_trigger == TriggerType::WhenAnOpponentHolds) {
            const GameObjectId card = triggerSubject(ctx);
            if (card == kInvalidId || !ctx.state.objectExists(card)) return;
            auto& o = ctx.state.getObject(card);
            auto& ban = ctx.state.player(o.owner).banishment;
            auto it = std::find(ban.begin(), ban.end(), card);
            if (o.zone != ZoneType::Banishment || it == ban.end()) return;   // it left banishment
            ban.erase(it);
            o.zone = ZoneType::Hand;
            o.location = std::nullopt;
            ctx.state.player(o.owner).hand.push_back(card);
            ctx.events.logTrace("ASHE FOCUSED: they held -> " + o.name + " returns to their hand");
            return;
        }
        PlayerId opp = opponent(ctx.controller);
        auto& opp_ps = ctx.state.player(opp);

        // Reveal their hand.
        for (auto cid : opp_ps.hand) {
            if (!ctx.state.objectExists(cid)) continue;
            const auto& c = ctx.state.getObject(cid);
            ctx.events.emit(CardRevealedEvent{
                /*card=*/cid,
                /*card_def_id=*/c.card_def_id,
                /*owner=*/c.owner,
                /*revealed_to_all=*/false,
                /*revealed_to=*/ctx.controller,
                /*source_zone=*/ZoneType::Hand,
            });
        }
        ctx.events.logTrace("ASHE FOCUSED: opponent reveals hand (" +
                            std::to_string(opp_ps.hand.size()) + " card(s))");

        // Choose a revealed card and banish it.
        std::vector<GameObjectId> legal(opp_ps.hand.begin(), opp_ps.hand.end());
        if (legal.empty()) return;
        GameObjectId picked = pickTarget(ctx, "Ashe Focused: banish a revealed card",
                                         legal);
        if (picked == kInvalidId || !ctx.state.objectExists(picked)) return;
        // Remove from hand, then banish.
        auto& h = ctx.state.player(opp).hand;
        h.erase(std::remove(h.begin(), h.end(), picked), h.end());
        ctx.executor.banishObject(picked);
        DelayedAbility da;
        da.source = ctx.source;
        da.card_def_id = cardDefId();
        da.controller = ctx.controller;
        da.trigger = TriggerType::WhenAnOpponentHolds;
        da.payload = picked;
        ctx.state.delayed_abilities.push_back(da);
        ctx.events.logTrace("ASHE FOCUSED: banished a card from the opponent's hand until they hold");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 731;
        d.def_id = R"RB(unl-169-219)RB";
        d.name = R"RB(Ashe, Focused)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-169/219)RB";
        d.collector_number = 169;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Order};
        d.tags = {R"RB(Freljord)RB", R"RB(Ashe)RB"};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 4;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play me, choose an opponent. They reveal their hand. Choose a card revealed this way and banish it. When they hold, return it to their hand (even if I'm no longer on the board).)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ebc033772f2005b7f39ba87ba9ea35f43df0da7a-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_731(CardRegistry& r) {
    r.registerCard(731, std::make_unique<AsheFocused>());
}

} // namespace riftbound
