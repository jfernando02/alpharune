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

class ResourceExtractor : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] At the end of your turn, if you control this battlefield, play a
    // Gold gear token exhausted."
    TriggerType triggerType() const override { return TriggerType::AtEndOfTurn; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId p, TriggerType, GameObjectId) const override {
        if (!s.objectExists(self)) return false;
        const auto bf = s.getObject(self).battlefieldId();
        if (!bf) return false;
        for (const auto& b : s.battlefields)
            if (b.id == *bf) return b.controller.has_value() && *b.controller == p;
        return false;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        createGoldExhausted(ctx);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 995;
        d.def_id = R"RB(rad-052-167)RB";
        d.name = R"RB(Resource Extractor)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-052/167)RB";
        d.collector_number = 52;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Mind};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
At the end of your turn, if you control this battlefield, play a Gold gear token exhausted. (It has "[Reaction][>] Kill this, [E]: [Add] [A]."))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/d5cc2f85febe2260e51b88bddfeab6ce700c85dd-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_995(CardRegistry& r) {
    r.registerCard(995, std::make_unique<ResourceExtractor>());
}

}  // namespace riftbound
