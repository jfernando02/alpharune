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

class ClangingSentry : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "Your Mechs have [Disarm]." Each Sentry gives an attacking Mech Disarm 1.
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenAFriendlyUnitAttacks; }
    bool triggerCondition(const GameState& s, GameObjectId, PlayerId, TriggerType, GameObjectId subject) const override {
        return subject != kInvalidId && s.objectExists(subject) && hasTag(s.getObject(subject), "Mech");
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        resolveDisarm(*this, ctx, triggerSubject(ctx), 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 997;
        d.def_id = R"RB(rad-055-167)RB";
        d.name = R"RB(Clanging Sentry)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-055/167)RB";
        d.collector_number = 55;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Mind};
        d.energy_cost = 3;
        d.might = 2;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(Your Mechs have [Disarm]. (When each of us attacks, give an enemy unit there -1 [M] this turn.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5d4c22ef863e041887a00d34b2dae1f3ce658326-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_997(CardRegistry& r) {
    r.registerCard(997, std::make_unique<ClangingSentry>());
}

}  // namespace riftbound
