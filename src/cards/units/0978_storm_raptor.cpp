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

class StormRaptor : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deflect] When a stunned enemy unit dies here, draw 1."
    TriggerType triggerType() const override { return TriggerType::WhenAnEnemyUnitDiesHere; }
    bool triggerCondition(const GameState& s, GameObjectId, PlayerId, TriggerType, GameObjectId subject) const override {
        return subject != kInvalidId && s.objectExists(subject) && s.getObject(subject).is_stunned;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 978;
        d.def_id = R"RB(rad-036-167)RB";
        d.name = R"RB(Storm Raptor)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-036/167)RB";
        d.collector_number = 36;
        d.artist = R"RB(Bubble Cat Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 6;
        d.might = 6;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Deflect);
        d.deflect_value = 1;
        d.ability_text = R"RB([Deflect] (Opponents must pay [A] to choose me with a spell or ability.)
When a stunned enemy unit dies here, draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/fc59661dfc70a1fdb393d795c90779a2b5661609-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_978(CardRegistry& r) {
    r.registerCard(978, std::make_unique<StormRaptor>());
}

}  // namespace riftbound
