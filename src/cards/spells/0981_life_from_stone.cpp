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

// "As an additional cost to play this, exhaust a friendly unit. Draw 1 and
// channel 1 rune exhausted." Gated on a ready friendly unit; the unit is
// chosen and exhausted when the spell is finalized (CR 355).
class LifeFromStone : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> readyFriendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value() &&
                !obj.is_exhausted)
                out.push_back(id);
        return out;
    }
    bool canBePlayed(const GameState& state, PlayerId player) const override {
        return !readyFriendlyUnits(state, player).empty();
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId paid = pickTarget(ctx, "Life from Stone: exhaust a friendly unit",
                                             costChoices(ctx, readyFriendlyUnits(ctx.state, ctx.controller)));
        if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
        if (paid != kInvalidId && ctx.state.objectExists(paid) && costDueNow(ctx))
            ctx.executor.exhaustObject(paid);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (paid == kInvalidId && !costAlreadyPaid(ctx)) return;  // the cost was never paid
        ctx.executor.drawCards(ctx.controller, 1);
        ctx.executor.channelRunes(ctx.controller, 1, /*enter_exhausted=*/true);
        ctx.events.logTrace("LIFE FROM STONE: exhausted a friendly unit -> draw 1, channel 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 981;
        d.def_id = R"RB(rad-029-167)RB";
        d.name = R"RB(Life from Stone)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-029/167)RB";
        d.collector_number = 29;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
As an additional cost to play this, exhaust a friendly unit.
Draw 1 and channel 1 rune exhausted.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/6a849c0425f0fecb8d00e1b9494e975c4dbbba75-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_981(CardRegistry& r) {
    r.registerCard(981, std::make_unique<LifeFromStone>());
}

} // namespace riftbound
