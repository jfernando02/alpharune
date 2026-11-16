#include "cards/card.h"
#include "cards/card_registry.h"
#include "cards/card_helpers.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class BattleMistress : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenAnEnemyUnitDies, TriggerType::WhenYouRecycleARune};
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    // "one or more": once for units that die at the same time.
    bool triggerCondition(const GameState& state, GameObjectId /*source*/, PlayerId controller,
                          TriggerType trigger, GameObjectId subject) const override {
        if (trigger != TriggerType::WhenAnEnemyUnitDies) return true;
        return firstDeathInBatch(state, opponent(controller), subject);
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>&) override {
        if (ctx.firing_trigger == TriggerType::WhenYouRecycleARune) {
            // "When you recycle a rune, you may exhaust me to play a Gold gear
            // token exhausted."
            auto ready = [&] {
                return ctx.state.objectExists(ctx.source) && !ctx.state.getObject(ctx.source).is_exhausted;
            };
            const int go = confirmOptional(ctx, "Battle Mistress: exhaust me to play a Gold token?",
                                           [&] { return costAlreadyPaid(ctx) || ready(); });
            if (go == -1) return;
            if (go == 1 && costDueNow(ctx)) ctx.executor.exhaustObject(ctx.source);
            if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
            if (go != 1) return;
            createGoldExhausted(ctx);
            ctx.events.logTrace("BATTLE MISTRESS: exhausted -> Gold token");
            return;
        }
        // "When one or more enemy units die, ready me."
        if (finalizingChoices(ctx)) return;
        if (!ctx.state.objectExists(ctx.source)) return;
        ctx.executor.readyObject(ctx.source);
        ctx.events.logTrace("BATTLE MISTRESS: enemy unit died -> ready me");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 517;
        d.def_id = R"RB(sfd-203-221)RB";
        d.name = R"RB(Battle Mistress)RB";
        d.set_code = R"RB(SFD)RB";
        d.set_name = R"RB(Spiritforged)RB";
        d.public_code = R"RB(SFD-203/221)RB";
        d.collector_number = 203;
        d.artist = R"RB(Zhongqi Li)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Body, Domain::Chaos};
        d.tags = {R"RB(Sivir)RB"};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you recycle a rune, you may exhaust me to play a Gold gear token exhausted.
When one or more enemy units die, ready me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/fd060882c32a8deac04aea4241c6ab7b97236a05-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_517(CardRegistry& r) {
    r.registerCard(517, std::make_unique<BattleMistress>());
}

} // namespace riftbound
