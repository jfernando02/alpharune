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

class BlackRoseSanctum : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    // "For each player, the first time each turn a non-token unit they control
    // dies here, they play a 1 [M] Recruit unit token to their base."
    TriggerType triggerType() const override { return TriggerType::WhenAUnitDiesHere; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId who, TriggerType, GameObjectId subject) const override {
        if (!s.objectExists(self) || subject == kInvalidId || !s.objectExists(subject)) return false;
        if (s.getObject(subject).is_token) return false;
        const auto& c = s.getObject(self).card_counters;
        const auto it = c.find(who == PlayerId::Player1 ? "__brs_p1" : "__brs_p2");
        return it == c.end() || it->second != s.turn.turn_number;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.state.objectExists(ctx.source))
            ctx.state.getObject(ctx.source).card_counters[ctx.controller == PlayerId::Player1 ? "__brs_p1" : "__brs_p2"] =
                ctx.state.turn.turn_number;
        createRecruitAtBase(ctx, ctx.controller);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1078;
        d.def_id = R"RB(rad-159-167)RB";
        d.name = R"RB(Black Rose Sanctum)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-159/167)RB";
        d.collector_number = 159;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(For each player, the first time each turn a non-token unit they control dies here, they play a 1 [M] Recruit unit token to their base.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/03ce18a231c7c919875b436b06bbe4e1a99c4831-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1078(CardRegistry& r) {
    r.registerCard(1078, std::make_unique<BlackRoseSanctum>());
}

}  // namespace riftbound
