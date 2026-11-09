#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

// "When you conquer here, you may discard 1 and channel 1 rune exhausted."
// Whether to do it is decided when the trigger is finalized; the card to
// discard is picked as it resolves.
class RuinedMonastery : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouConquerHere; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const PlayerId me = ctx.controller;
        auto can = [&]() {
            return !ctx.state.player(me).hand.empty() && !ctx.state.player(me).rune_deck.empty();
        };
        const int go = confirmOptional(ctx, "Ruined Monastery: discard 1 and channel 1 rune exhausted?",
                                       can);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (go != 1 || !can()) return;
        const auto hand = ctx.state.player(me).hand;
        GameObjectId card = pickTarget(ctx, "Ruined Monastery: discard 1", hand);
        if (ctx.executor.hasPendingChoice()) return;   // waiting for the pick
        if (card == kInvalidId || !ctx.state.objectExists(card)) return;
        ctx.executor.applyDiscard(me, card);
        ctx.executor.channelRunes(me, 1, /*enter_exhausted=*/true);
        ctx.events.logTrace("RUINED MONASTERY: discarded 1, channelled 1 rune exhausted");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1098;
        d.def_id = R"RB(rad-166-167)RB";
        d.name = R"RB(Ruined Monastery)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-166/167)RB";
        d.collector_number = 166;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you conquer here, you may discard 1 and channel 1 rune exhausted.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/2d756c5198dafb151f51cceca4f11824575960cd-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1098(CardRegistry& r) {
    r.registerCard(1098, std::make_unique<RuinedMonastery>());
}

} // namespace riftbound
