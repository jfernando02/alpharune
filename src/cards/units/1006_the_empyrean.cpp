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
// APPROX: the engine has no [Show Off] step yet. A card can always be shown
// off (a card in hand, or another friendly game object — a rune at the least),
// and showing off reveals nothing worth hiding next to an 8-cost play, so the
// card is taken as shown off, with the highest Power cost on offer.
class TheEmpyrean : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        const auto& db = ctx.executor.cardDB();
        auto power_of = [&](const GameObject& o) {
            return o.card_def_id == kInvalidId ? 0 : db.get(o.card_def_id).power_cost;
        };
        bool any = false;
        int best = 0;
        for (auto cid : ctx.state.player(me).hand) {
            if (!ctx.state.objectExists(cid)) continue;
            any = true;
            best = std::max(best, power_of(ctx.state.getObject(cid)));
        }
        for (const auto& [id, o] : ctx.state.objects) {
            if (id == ctx.source || o.controller != me || !o.location.has_value()) continue;
            any = true;
            best = std::max(best, power_of(o));
        }
        if (!any) return;   // nothing to show off
        if (best > 0) ctx.executor.predict(me, best);
        ctx.executor.drawCards(me, 2);
        ctx.events.logTrace("THE EMPYREAN: showed off a card -> predict " +
                            std::to_string(best) + ", draw 2");
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
        d.keywords.set(Keyword::Predict);
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
