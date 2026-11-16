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

class LastCaress : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "The next unit you play this turn (even from face down) can be played to
    // any battlefield. When you play it, [Buff] it."
    // APPROX: the any-battlefield permission is not modeled.
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        DelayedAbility da;
        da.source = ctx.source;
        da.card_def_id = cardDefId();
        da.controller = ctx.controller;
        da.trigger = TriggerType::WhenYouPlayAUnit;
        da.expires_on_turn = ctx.state.turn.turn_number;
        ctx.state.delayed_abilities.push_back(da);
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto u = triggerSubject(ctx);
        if (u != kInvalidId && ctx.state.objectExists(u) && ctx.state.getObject(u).location) ctx.executor.buffUnit(u);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1072;
        d.def_id = R"RB(rad-154-167)RB";
        d.name = R"RB(Last Caress)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-154/167)RB";
        d.collector_number = 154;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Body, Domain::Chaos};
        d.energy_cost = 2;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
The next unit you play this turn (even from face down) can be played to any battlefield. When you play it, [Buff] it. (Give it a +1 [M] buff if it doesn't have one.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/c34881b6fe2e63f6345845fe95cfe72eb9d4878c-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1072(CardRegistry& r) {
    r.registerCard(1072, std::make_unique<LastCaress>());
}

}  // namespace riftbound
