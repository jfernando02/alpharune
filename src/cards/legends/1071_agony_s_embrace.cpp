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

class AgonysEmbrace : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "Once each turn, when you play a card from face down, draw 1. Then, if
    // it's not your turn, discard 1."
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayFromFacedown; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId, TriggerType, GameObjectId) const override {
        if (!s.objectExists(self)) return false;
        const auto& c = s.getObject(self).card_counters;
        const auto it = c.find("__ae_turn");
        return it == c.end() || it->second != s.turn.turn_number;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        if (ri.resume_point == 0) {
            if (ctx.state.objectExists(ctx.source))
                ctx.state.getObject(ctx.source).card_counters["__ae_turn"] = ctx.state.turn.turn_number;
            ctx.executor.drawCards(ctx.controller, 1);
            if (ctx.state.turn.turn_player == ctx.controller) return;
        }
        discardOneThenAct(ctx, "Agony's Embrace: discard 1", [](CardContext&, GameObjectId) {});
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1071;
        d.def_id = R"RB(rad-153-167)RB";
        d.name = R"RB(Agony's Embrace)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-153/167)RB";
        d.collector_number = 153;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Body, Domain::Chaos};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(Once each turn, when you play a card from face down, draw 1. Then, if it's not your turn, discard 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ca217b57d0e8d2e04e7a35105deaa6be17e854f0-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1071(CardRegistry& r) {
    r.registerCard(1071, std::make_unique<AgonysEmbrace>());
}

}  // namespace riftbound
