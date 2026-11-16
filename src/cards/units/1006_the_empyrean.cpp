#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include "core/card_db.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "[Show Off] a card. When you play me, if you showed off a card, [Predict]
//  equal to its Power cost, then draw 2."
// The player picks the card shown off (a card in hand or another friendly one).
class TheEmpyrean : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // [Show Off] is chosen as I am played: the player picks a card in hand or
    // another friendly card on the board, and Predicts its Power cost.
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    static std::vector<GameObjectId> showable(const GameState& s, PlayerId me, GameObjectId self) {
        std::vector<GameObjectId> out;
        for (auto cid : s.player(me).hand)
            if (cid != self && s.objectExists(cid)) out.push_back(cid);
        for (const auto& [id, o] : s.objects)
            if (id != self && o.controller == me && o.location.has_value()) out.push_back(id);
        return out;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        const auto cards = showable(ctx.state, me, ctx.source);
        if (cards.empty()) { if (finalizingChoices(ctx)) return; return; }   // nothing to show off
        const GameObjectId shown = pickTarget(ctx, "The Empyrean: show off a card", cards);
        if (ctx.executor.hasPendingChoice()) return;   // waiting for the pick
        if (finalizingChoices(ctx)) return;            // choices end here (CR 383.3)
        if (shown == kInvalidId || !ctx.state.objectExists(shown)) return;
        const auto& o = ctx.state.getObject(shown);
        const int power = o.card_def_id == kInvalidId ? 0 : ctx.executor.cardDB().get(o.card_def_id).power_cost;
        if (power > 0) ctx.executor.predict(me, power);
        ctx.executor.drawCards(me, 2);
        ctx.events.logTrace("THE EMPYREAN: showed off " + o.name + " -> predict " +
                            std::to_string(power) + ", draw 2");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1006;
        d.def_id = R"RB(rad-060-167)RB";
        d.name = R"RB(The Empyrean)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-060/167)RB";
        d.collector_number = 60;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Mind};
        d.tags = {R"RB(Dragon)RB"};
        d.energy_cost = 8;
        d.might = 8;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB([Show Off] a card. (As you play me, you may reveal a card from hand or pick another friendly one.)
When you play me, if you showed off a card, [Predict] equal to its Power cost, then draw 2. (Look at that many cards from your Main Deck. Recycle any of them and put the rest back in any order.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b7cd2e475834863bf4e66994118a835ba4377d16-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1006(CardRegistry& r) {
    r.registerCard(1006, std::make_unique<TheEmpyrean>());
}

} // namespace riftbound
