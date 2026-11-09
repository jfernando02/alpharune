#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "[Accelerate] When I become exhausted, choose one — Play a Gold gear token
//  exhausted. / Pay [C] to draw 1."
// The mode and the [C] payment are settled when the trigger is finalized.
class RecklessInnovator : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIBecomeExhausted; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        uint32_t legal = 1u;
        if (payableOnePower(ctx.state, me, Domain::Chaos)) legal |= 2u;
        const int mode = pickMode(ctx, "Reckless Innovator", 2,
                                  {"play a Gold gear token exhausted", "pay [C] to draw 1"}, legal);
        if (mode == -1) return;   // waiting for the agent
        bool paid = false;
        if (mode == 1 && costDueNow(ctx)) paid = payOnePower(ctx, me, Domain::Chaos);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (mode == 0) {
            createGoldExhausted(ctx);
            ctx.events.logTrace("RECKLESS INNOVATOR: played a Gold gear token exhausted");
        } else if (mode == 1 && (paid || costAlreadyPaid(ctx))) {
            ctx.executor.drawCards(me, 1);
            ctx.events.logTrace("RECKLESS INNOVATOR: paid [C] -> draw 1");
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1043;
        d.def_id = R"RB(rad-105-167)RB";
        d.name = R"RB(Reckless Innovator)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-105/167)RB";
        d.collector_number = 105;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Accelerate);
        d.ability_text = R"RB([Accelerate] (You may pay [1][C] as an additional cost to have me enter ready.)
When I become exhausted, choose one —
- Play a Gold gear token exhausted. (It has "[Reaction][>] Kill this, [E]: [Add] [A].")
- Pay [C] to draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b7ff5684309bcd346e6b3534e0d1f38ec1a38726-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1043(CardRegistry& r) {
    r.registerCard(1043, std::make_unique<RecklessInnovator>());
}

} // namespace riftbound
