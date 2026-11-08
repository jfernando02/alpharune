#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class OrnnBlacksmith : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayMe, TriggerType::WhenIHold};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>&) override {
        auto& ps = ctx.state.player(ctx.controller);

        // Look at the top 4 (top = back of the vector). They stay in the deck
        // until the pick is made, so the trigger can suspend for the choice.
        std::vector<GameObjectId> peeked;
        for (int i = static_cast<int>(ps.main_deck.size()) - 1;
             i >= 0 && static_cast<int>(peeked.size()) < 4; --i)
            peeked.push_back(ps.main_deck[i]);
        if (peeked.empty()) return;

        // "You may reveal a gear from among them and draw it": the agent picks
        // which gear (drawing one is never worse than recycling it).
        std::vector<GameObjectId> gears;
        for (auto id : peeked)
            if (ctx.state.objectExists(id) && ctx.state.getObject(id).isGear())
                gears.push_back(id);
        GameObjectId drafted = kInvalidId;
        if (!gears.empty()) {
            drafted = pickTarget(ctx, "Ornn, Blacksmith: reveal a gear and draw it", gears);
            if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
        }

        auto takeFromDeck = [&](GameObjectId id) {
            auto it = std::find(ps.main_deck.begin(), ps.main_deck.end(), id);
            if (it == ps.main_deck.end()) return false;
            ps.main_deck.erase(it);
            return true;
        };
        if (drafted != kInvalidId && ctx.state.objectExists(drafted) && takeFromDeck(drafted)) {
            auto& drafted_obj = ctx.state.getObject(drafted);
            ctx.events.emit(CardRevealedEvent{
                /*card=*/drafted,
                /*card_def_id=*/drafted_obj.card_def_id,
                /*owner=*/drafted_obj.owner,
                /*revealed_to_all=*/true,
                /*revealed_to=*/ctx.controller,
                /*source_zone=*/ZoneType::MainDeck,
            });
            ctx.events.logTrace("ORNN, BLACKSMITH: drafted gear " +
                                 drafted_obj.name);
            drafted_obj.zone = ZoneType::Hand;
            ps.hand.push_back(drafted);
        }

        // Recycle the rest to the bottom of the deck.
        std::vector<GameObjectId> rest;
        for (auto id : peeked)
            if (id != drafted && takeFromDeck(id)) rest.push_back(id);
        if (!rest.empty()) {
            ctx.executor.recycleCards(ctx.controller, rest);
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 381;
        d.def_id = R"RB(sfd-058-221)RB";
        d.name = R"RB(Ornn, Blacksmith)RB";
        d.set_code = R"RB(SFD)RB";
        d.set_name = R"RB(Spiritforged)RB";
        d.public_code = R"RB(SFD-058/221)RB";
        d.collector_number = 58;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Calm};
        d.tags = {R"RB(Ornn)RB", R"RB(Freljord)RB"};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 5;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(When you play me or when I hold, look at the top 4 cards of your Main Deck. You may reveal a gear from among them and draw it. Then recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/77b48a28f48f26714f4bcf860379945a2d7186aa-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_381(CardRegistry& r) {
    r.registerCard(381, std::make_unique<OrnnBlacksmith>());
}

} // namespace riftbound
