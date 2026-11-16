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

class RavingWitness : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When I move, reveal the top card of your Main Deck. If it's [Mighty], draw
    // it. Otherwise, recycle it."
    TriggerType triggerType() const override { return TriggerType::WhenIMove; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        if (ps.main_deck.empty()) return;
        const auto top = ps.main_deck.back();
        const auto& c = ctx.state.getObject(top);
        ctx.events.emit(CardRevealedEvent{top, c.card_def_id, c.owner, true, ctx.controller, ZoneType::MainDeck});
        const bool mighty = c.isUnit() && c.base_might >= 5;
        if (mighty) { ctx.executor.drawCards(ctx.controller, 1); return; }
        ps.main_deck.pop_back();
        ps.main_deck.insert(ps.main_deck.begin(), top);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1014;
        d.def_id = R"RB(rad-074-167)RB";
        d.name = R"RB(Raving Witness)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-074/167)RB";
        d.collector_number = 74;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Body};
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When I move, reveal the top card of your Main Deck. If it's [Mighty], draw it. Otherwise, recycle it. (A unit is Mighty while it has 5+ [M].))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/778e70b353d7f28eae9b54b1a37af3853238ba04-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1014(CardRegistry& r) {
    r.registerCard(1014, std::make_unique<RavingWitness>());
}

}  // namespace riftbound
