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

class TitanicShrine : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a unit with Might greater than each other unit on the board,
    // exhaust me to draw 1."
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayAUnit; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId, TriggerType, GameObjectId subject) const override {
        if (!s.objectExists(self) || s.getObject(self).is_exhausted) return false;
        if (subject == kInvalidId || !s.objectExists(subject)) return false;
        const int m = s.getObject(subject).current_might;
        for (const auto& [id, o] : s.objects)
            if (id != subject && o.isUnit() && o.location.has_value() && o.current_might >= m) return false;
        return true;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source) || ctx.state.getObject(ctx.source).is_exhausted) return;
        ctx.executor.exhaustObject(ctx.source);
        ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1026;
        d.def_id = R"RB(rad-089-167)RB";
        d.name = R"RB(Titanic Shrine)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-089/167)RB";
        d.collector_number = 89;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Body};
        d.energy_cost = 3;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play a unit with Might greater than each other unit on the board, exhaust me to draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/7d027f4e30df4f7141799709f9fb785835adefed-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1026(CardRegistry& r) {
    r.registerCard(1026, std::make_unique<TitanicShrine>());
}

}  // namespace riftbound
