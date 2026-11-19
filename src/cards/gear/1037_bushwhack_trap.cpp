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

class BushwhackTrap : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deploy] While you or an ally control this battlefield, that player may
    // hide an additional card here."
    // One more slot per Trap its controller has at a battlefield they control.
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        for (auto& bf : state.battlefields) {
            if (!bf.controller.has_value() || *bf.controller != controller) continue;
            int n = 0;
            for (const auto& [id, o] : state.objects)
                if (o.card_def_id == cardDefId() && o.controller == controller &&
                    o.battlefieldId() == bf.id) ++n;
            bf.extra_hide_slots = n;
        }
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1037;
        d.def_id = R"RB(rad-108-167)RB";
        d.name = R"RB(Bushwhack Trap)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-108/167)RB";
        d.collector_number = 108;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Chaos};  // the gallery says Fury (a mislabel): its collector number is in the Chaos block, as riftdecks lists it
        d.energy_cost = 1;
        d.rarity = Rarity::Rare;
        d.deploy = true;
        d.ability_text = R"RB([Deploy] (Play this only to a battlefield. When an opponent holds here, kill this.)
While you or an ally control this battlefield, that player may hide an additional card here.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/120762e23a599ee14d3f88856fcf707154338fb0-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1037(CardRegistry& r) {
    r.registerCard(1037, std::make_unique<BushwhackTrap>());
}

}  // namespace riftbound
