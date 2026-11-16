#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "[Hidden] When you play me, choose a battlefield. If there are 4 or more
//  units there, you may return an enemy unit there to its owner's hand."
// The battlefield and the unit come down to one pick: an enemy unit at a
// battlefield holding 4+ units, chosen when the trigger is finalized.
// The "you may" is asked.
class BedevilingStranger : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    static std::vector<GameObjectId> returnable(const GameState& state, PlayerId me) {
        std::map<BattlefieldId, int> units_at;
        for (const auto& [id, o] : state.objects)
            if (o.isUnit() && o.location.has_value())
                if (auto bf = o.battlefieldId()) ++units_at[*bf];
        return chooseableUnits(state, me, [&](const GameObject& o) {
            if (o.controller == me) return false;
            auto bf = o.battlefieldId();
            return bf.has_value() && units_at[*bf] >= 4;
        });
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto legal = returnable(ctx.state, ctx.controller);   // "4 or more" checked again on resolution
        const int go = confirmOptional(ctx, "Bedeviling Stranger: return an enemy unit to hand?",
                                       [&] { return !returnable(ctx.state, ctx.controller).empty(); });
        if (go == -1) return;   // waiting for the answer
        GameObjectId t = (go == 0 || legal.empty()) ? kInvalidId
            : pickTarget(ctx, "Bedeviling Stranger: return an enemy unit to hand", legal);

        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (t == kInvalidId || !ctx.state.objectExists(t)) return;
        if (!ctx.state.getObject(t).location.has_value()) return;
        ctx.executor.bounceToHand(t);
        ctx.events.logTrace("BEDEVILING STRANGER: returned an enemy unit to its owner's hand");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1045;
        d.def_id = R"RB(rad-107-167)RB";
        d.name = R"RB(Bedeviling Stranger)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-107/167)RB";
        d.collector_number = 107;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};
        d.energy_cost = 2;
        d.might = 2;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Hidden);
        d.ability_text = R"RB([Hidden] (Hide now for [A] to react with later for [0].)
When you play me, choose a battlefield. If there are 4 or more units there, you may return an enemy unit there to its owner's hand.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/d112a706fe957e5cea58ea1e4366ac02e3ba292b-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1045(CardRegistry& r) {
    r.registerCard(1045, std::make_unique<BedevilingStranger>());
}

} // namespace riftbound
