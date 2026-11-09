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

// "[Action] Kill a gear. Its controller plays a Gold gear token exhausted."
class AnOfferYouCantRefuse : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_gear = true};
    }
    bool needsPlayTimeTarget() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto legal = enumerateLegalTargets(ctx.state, ctx.controller);
        GameObjectId gear = pickTarget(ctx, "An Offer You Can't Refuse: kill a gear", legal);
        if (gear == kInvalidId || !ctx.state.objectExists(gear)) return;
        const PlayerId gear_controller = ctx.state.getObject(gear).controller;
        ctx.executor.unattachGear(gear);   // no-op unless it is equipped
        ctx.executor.killObject(gear);
        createGoldExhausted(ctx, gear_controller);
        ctx.events.logTrace("AN OFFER YOU CAN'T REFUSE: killed a gear; its controller gets a Gold");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1041;
        d.def_id = R"RB(rad-101-167)RB";
        d.name = R"RB(An Offer You Can't Refuse)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-101/167)RB";
        d.collector_number = 101;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Chaos};
        d.energy_cost = 2;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Kill a gear. Its controller plays a Gold gear token exhausted. (It has "[Reaction][>] Kill this, [E]: [Add] [A]."))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/f724f95257abeacee82c774eff64973b56035357-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1041(CardRegistry& r) {
    r.registerCard(1041, std::make_unique<AnOfferYouCantRefuse>());
}

} // namespace riftbound
