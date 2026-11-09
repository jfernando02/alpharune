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

// "For each player, the first time they play a unit or gear here each turn,
//  they [Predict]."
// Gear is always played to base in this engine, so only units land here.
// __rakelstake_p<i> = 1 + the turn player i last predicted here.
class Rakelstake : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayAUnit; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        auto& self = ctx.state.getObject(ctx.source);
        const std::string key = "__rakelstake_p" + std::to_string(playerIndex(ctx.controller));
        const int mark = ctx.state.turn.turn_number + 1;
        if (self.card_counters[key] == mark) return;   // already this turn
        self.card_counters[key] = mark;
        ctx.executor.predict(ctx.controller, 1);
        ctx.events.logTrace("RAKELSTAKE: first unit played here this turn -> predict");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1097;
        d.def_id = R"RB(rad-165-167)RB";
        d.name = R"RB(Rakelstake)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-165/167)RB";
        d.collector_number = 165;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Predict);
        d.ability_text = R"RB(For each player, the first time they play a unit or gear here each turn, they [Predict]. (They look at the top card of their Main Deck. They may recycle it.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/53c096e4bc843a4cfe84fd16d922033b7fcfb330-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1097(CardRegistry& r) {
    r.registerCard(1097, std::make_unique<Rakelstake>());
}

} // namespace riftbound
