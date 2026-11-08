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

// "Choose one — Move a friendly gear with [Deploy] to a battlefield. / Look at
// the top 5 cards of your Main Deck. You may reveal a gear from among them and
// draw it. Recycle the rest."
// APPROX: the engine has no [Deploy] gear yet, so the first mode can never be
// chosen and the card always looks for a gear. Drawing the gear is taken
// whenever there is one (the "you may" is never worth declining).
class CommandAttack : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        std::vector<GameObjectId> top;  // top = back of the deck vector
        for (int i = static_cast<int>(ps.main_deck.size()) - 1;
             i >= 0 && static_cast<int>(top.size()) < 5; --i)
            top.push_back(ps.main_deck[i]);
        if (top.empty()) return;
        std::vector<GameObjectId> gears;
        for (GameObjectId c : top)
            if (ctx.state.objectExists(c) && ctx.state.getObject(c).isGear()) gears.push_back(c);

        GameObjectId drawn = kInvalidId;
        if (!gears.empty()) {
            drawn = pickTarget(ctx, "Command: Attack: reveal a gear and draw it", gears);
            if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
        }
        if (drawn != kInvalidId && ctx.state.objectExists(drawn)) {
            auto& deck = ps.main_deck;
            deck.erase(std::remove(deck.begin(), deck.end(), drawn), deck.end());
            auto& obj = ctx.state.getObject(drawn);
            obj.zone = ZoneType::Hand;
            obj.location = std::nullopt;
            ps.hand.push_back(drawn);
            ctx.events.logTrace("COMMAND: ATTACK: revealed and drew " + obj.name);
        }
        std::vector<GameObjectId> rest;
        for (GameObjectId c : top) {
            if (c == drawn) continue;
            auto it = std::find(ps.main_deck.begin(), ps.main_deck.end(), c);
            if (it == ps.main_deck.end()) continue;
            ps.main_deck.erase(it);
            rest.push_back(c);
        }
        if (!rest.empty()) ctx.executor.recycleCards(ctx.controller, rest);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 983;
        d.def_id = R"RB(rad-033-167)RB";
        d.name = R"RB(Command: Attack)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-033/167)RB";
        d.collector_number = 33;
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(Choose one —
Move a friendly gear with [Deploy] to a battlefield.
Look at the top 5 cards of your Main Deck. You may reveal a gear from among them and draw it. Recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b025756992e22af480902e13b01e25acbc747221-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_983(CardRegistry& r) {
    r.registerCard(983, std::make_unique<CommandAttack>());
}

} // namespace riftbound
