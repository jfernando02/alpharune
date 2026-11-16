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

class MordekaiserBattlefieldTyrant : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Accelerate] [Assault 3] Once each turn, if I would die, you may kill a
    // friendly unit to heal me, exhaust me, and recall me instead."
    // APPROX: the death replacement is not modeled.
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 972;
        d.def_id = R"RB(rad-020-167)RB";
        d.name = R"RB(Mordekaiser, Twice Slain)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-020/167)RB";
        d.collector_number = 20;
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Fury};
        d.energy_cost = 7;
        d.power_cost = 1;
        d.might = 5;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Accelerate);
        d.keywords.set(Keyword::Assault);
        d.assault_value = 3;
        d.ability_text = R"RB([Accelerate] (You may pay [1][R] as an additional cost to have me enter ready.)
[Assault 3] (+3 [M] while I'm an attacker.)
Once each turn, if I would die, you may kill a friendly unit to heal me, exhaust me, and recall me instead.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/66db9510de566ac43157ecdcc96f33b5f553a38d-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_972(CardRegistry& r) {
    r.registerCard(972, std::make_unique<MordekaiserBattlefieldTyrant>());
}

}  // namespace riftbound
