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

class DrakehoundPackmaster : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, reveal the top card of each opponent's Main Deck. Choose
    // one and banish it, then play it, ignoring its cost. Recycle the rest."
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId opp = opponent(ctx.controller);
        auto& ops = ctx.state.player(opp);
        if (ops.main_deck.empty()) return;
        const auto top = ops.main_deck.back();
        ops.main_deck.pop_back();
        auto& c = ctx.state.getObject(top);
        ctx.events.emit(CardRevealedEvent{top, c.card_def_id, c.owner, true, ctx.controller, ZoneType::MainDeck});
        c.zone = ZoneType::Banishment;
        c.location = std::nullopt;
        ctx.events.logTrace("DRAKEHOUND PACKMASTER: banish and play " + c.name);
        std::optional<LocationId> loc;
        if (c.card_def_id != kInvalidId && ctx.executor.cardDB().get(c.card_def_id).deploy &&
            !ctx.state.battlefields.empty())
            loc = LocationId{BattlefieldLocation{ctx.state.battlefields.front().id}};
        ctx.executor.playIgnoringCost(ctx.controller, top, loc);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 970;
        d.def_id = R"RB(rad-018-167)RB";
        d.name = R"RB(Drakehound Packmaster)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-018/167)RB";
        d.collector_number = 18;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};
        d.energy_cost = 9;
        d.power_cost = 2;
        d.might = 9;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play me, reveal the top card of each opponent's Main Deck. Choose one and banish it, then play it, ignoring its cost. Recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9ddc2759cf8b039bc1ee77e32326c5475cc001e8-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_970(CardRegistry& r) {
    r.registerCard(970, std::make_unique<DrakehoundPackmaster>());
}

}  // namespace riftbound
