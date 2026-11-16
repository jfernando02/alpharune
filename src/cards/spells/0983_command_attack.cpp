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
// Both modes; the "you may reveal" is asked.
class CommandAttack : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // The mode is chosen as the spell is played (CR 355): "move a [Deploy] gear"
    // is the yes of a confirmOptional, so the battlefield can use pickMode.
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> deployGear(const GameState& s, PlayerId me) {
        std::vector<GameObjectId> out;
        for (const auto& [id, o] : s.objects)
            if (o.isGear() && o.deploy && o.controller == me && o.location.has_value()) out.push_back(id);
        return out;
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int move = confirmOptional(ctx, "Command: Attack: move a friendly [Deploy] gear "
                                              "(no: look at the top 5)?",
                                         [&] { return !deployGear(ctx.state, ctx.controller).empty(); });
        if (move == -1) return;   // waiting for the mode
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (move == 1) {
            // The battlefield first (pickMode's resume points come before pickTarget's).
            auto notThere = [&](BattlefieldId b) {
                std::vector<GameObjectId> out;
                for (auto g : deployGear(ctx.state, ctx.controller)) {
                    const auto at = ctx.state.getObject(g).battlefieldId();
                    if (!at || *at != b) out.push_back(g);
                }
                return out;
            };
            const int bf = pickBattlefield(*this, ctx, "Command: Attack: move a gear to which battlefield",
                                           [&](BattlefieldId b) { return !notThere(b).empty(); });
            if (bf == -1) return;   // waiting for the battlefield
            if (bf < 0) return;
            const auto gears = notThere(static_cast<BattlefieldId>(bf));
            if (gears.empty()) return;
            const GameObjectId g = pickTarget(ctx, "Command: Attack: move which gear", gears);
            if (ctx.executor.hasPendingChoice()) return;
            if (g == kInvalidId || !ctx.state.objectExists(g)) return;
            ctx.executor.moveToBattlefield(g, static_cast<BattlefieldId>(bf));

            ctx.events.logTrace("COMMAND: ATTACK: moved " + ctx.state.getObject(g).name);
            return;
        }
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
        int reveal = 0;
        if (!gears.empty()) {   // "you may reveal a gear" (pickMode is free in this mode)
            reveal = pickMode(ctx, "Command: Attack: reveal a gear?", 2, {"no", "yes"});
            if (reveal == -1) return;
        }
        if (reveal == 1) {
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
