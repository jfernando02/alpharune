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

class MechanicalBandit : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "At the start of your Beginning Phase, each player reveals the top card of
    // their Main Deck. If you revealed a card with the lowest Energy cost, draw
    // it. Recycle the rest."
    TriggerType triggerType() const override { return TriggerType::AtStartOfBeginning; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller, opp = opponent(me);
        auto& mine = ctx.state.player(me).main_deck;
        auto& theirs = ctx.state.player(opp).main_deck;
        if (mine.empty()) return;
        const auto a = mine.back();
        const int ca = printedEnergy(ctx, ctx.state.getObject(a));
        ctx.events.emit(CardRevealedEvent{a, ctx.state.getObject(a).card_def_id, me, true, me, ZoneType::MainDeck});
        int cb = 1 << 20;
        if (!theirs.empty()) {
            const auto b = theirs.back();
            cb = printedEnergy(ctx, ctx.state.getObject(b));
            ctx.events.emit(CardRevealedEvent{b, ctx.state.getObject(b).card_def_id, opp, true, me, ZoneType::MainDeck});
            theirs.pop_back();
            theirs.insert(theirs.begin(), b);
        }
        if (ca <= cb) { ctx.executor.drawCards(me, 1); return; }
        mine.pop_back();
        mine.insert(mine.begin(), a);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1039;
        d.def_id = R"RB(rad-111-167)RB";
        d.name = R"RB(Mechanical Bandit)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-111/167)RB";
        d.collector_number = 111;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Chaos};  // the gallery says Fury (a mislabel): its collector number is in the Chaos block, as riftdecks lists it
        d.energy_cost = 4;
        d.power_cost = 1;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(At the start of your Beginning Phase, each player reveals the top card of their Main Deck. If you revealed a card with the lowest Energy cost, draw it. Recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/cb2a419b61b6f1e6d138c3c3b07b393a4037237b-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1039(CardRegistry& r) {
    r.registerCard(1039, std::make_unique<MechanicalBandit>());
}

}  // namespace riftbound
