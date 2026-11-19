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

class DisposalExpert : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "I enter ready if you control no other units. [Disarm 2]"
    bool entersReadyOnPlay(const GameState& s, PlayerId controller, const GameObject& self) const override {
        for (const auto& [id, o] : s.objects)
            if (id != self.id && o.isUnit() && o.controller == controller && o.location.has_value()) return false;
        return true;
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIAttack; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        resolveDisarm(*this, ctx, ctx.source, 2);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1028;
        d.def_id = R"RB(rad-093-167)RB";
        d.name = R"RB(Disposal Expert)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-093/167)RB";
        d.collector_number = 93;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};  // the gallery says Fury (a mislabel): its collector number is in the Chaos block, as riftdecks lists it
        d.energy_cost = 4;
        d.might = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(I enter ready if you control no other units.
[Disarm 2] (When I attack, give an enemy unit here -2 [M] this turn.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/79ec89753be773e7ea93b2116ea29d21c81fcef9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1028(CardRegistry& r) {
    r.registerCard(1028, std::make_unique<DisposalExpert>());
}

}  // namespace riftbound
