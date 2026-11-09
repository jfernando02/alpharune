#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "core/card_db.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "When you play me, look at the top 4 cards of your Main Deck. You may reveal
//  from among them a unit or gear with Energy cost no more than [3] and with
//  Power cost no more than [A] and draw it. Recycle the rest."
// APPROX: drawing a card is never worth declining, so a fitting card is always
// taken (the agent picks which).
class TrifarianCaptain : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        std::vector<GameObjectId> top;  // top = back of the deck vector
        for (int i = static_cast<int>(ps.main_deck.size()) - 1;
             i >= 0 && static_cast<int>(top.size()) < 4; --i)
            top.push_back(ps.main_deck[i]);
        if (top.empty()) return;
        const auto& db = ctx.executor.cardDB();
        std::vector<GameObjectId> fits;
        for (GameObjectId c : top) {
            if (!ctx.state.objectExists(c)) continue;
            const auto& o = ctx.state.getObject(c);
            if ((!o.isUnit() && !o.isGear()) || o.card_def_id == kInvalidId) continue;
            const auto& d = db.get(o.card_def_id);
            if (d.energy_cost <= 3 && d.power_cost <= 1) fits.push_back(c);
        }
        GameObjectId drawn = kInvalidId;
        if (!fits.empty()) {
            drawn = pickTarget(ctx, "Trifarian Captain: reveal a unit or gear and draw it", fits);
            if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
        }
        if (drawn != kInvalidId && ctx.state.objectExists(drawn)) {
            auto& deck = ps.main_deck;
            deck.erase(std::remove(deck.begin(), deck.end(), drawn), deck.end());
            auto& obj = ctx.state.getObject(drawn);
            obj.zone = ZoneType::Hand;
            obj.location = std::nullopt;
            ps.hand.push_back(drawn);
            ctx.events.logTrace("TRIFARIAN CAPTAIN: revealed and drew " + obj.name);
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
        d.id = 1064;
        d.def_id = R"RB(rad-129-167)RB";
        d.name = R"RB(Trifarian Captain)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-129/167)RB";
        d.collector_number = 129;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 2;
        d.might = 1;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you play me, look at the top 4 cards of your Main Deck. You may reveal from among them a unit or gear with Energy cost no more than [3] and with Power cost no more than [A] and draw it. Recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/914ced10400826a878ed5cf7294207a4e7b542b1-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1064(CardRegistry& r) {
    r.registerCard(1064, std::make_unique<TrifarianCaptain>());
}

} // namespace riftbound
