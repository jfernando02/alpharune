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

class ElnukHerd : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Hidden] When you play me, give another friendly unit +3 [M] this turn,
    // then recycle me if I was played from face down."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto t = pickTarget(ctx, "Elnuk Herd: +3 [M] to another friendly unit",
                                  dropSource(ctx.source, friendlyUnits(ctx.state, ctx.controller)));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t)) ctx.executor.giveTemporaryMight(t, 3);
        if (!ctx.state.objectExists(ctx.source)) return;
        auto& me = ctx.state.getObject(ctx.source);
        if (!me.card_counters.count("__played_from_facedown") || !me.location.has_value()) return;
        me.card_counters.erase("__played_from_facedown");
        const auto was = *me.location;
        me.location = std::nullopt;
        me.zone = ZoneType::MainDeck;
        auto& ps = ctx.state.player(me.owner);
        ps.main_deck.insert(ps.main_deck.begin(), ctx.source);
        ctx.events.emit(LeftBoardEvent{ctx.source, ctx.controller, CardType::Unit, was, ZoneType::MainDeck, false});
        ctx.events.logTrace("ELNUK HERD: recycled (played from face down)");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1019;
        d.def_id = R"RB(rad-081-167)RB";
        d.name = R"RB(Elnuk Herd)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-081/167)RB";
        d.collector_number = 81;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Body};  // the gallery says Fury (a mislabel): its collector number is in the Body block, as riftdecks lists it
        d.energy_cost = 6;
        d.might = 5;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Hidden);
        d.ability_text = R"RB([Hidden] (Hide now for [A] to react with later for [0].)
When you play me, give another friendly unit +3 [M] this turn, then recycle me if I was played from face down.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/1799cd4c46fcf0243b4022de2d427b15e67a15ad-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1019(CardRegistry& r) {
    r.registerCard(1019, std::make_unique<ElnukHerd>());
}

}  // namespace riftbound
