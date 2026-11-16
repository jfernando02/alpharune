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

class DuneDevourer : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "You may kill a friendly unit as an additional cost to play me. If you do,
    // I cost [2] less. [Deflect]"
    OptionalAdditionalCost optionalAdditionalCost() const override {
        OptionalAdditionalCost c;
        c.valid = true;
        c.kill_friendly_unit = true;
        c.reduce_energy = 2;
        c.paid_flag = "__dune_paid";
        return c;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1050;
        d.def_id = R"RB(rad-125-167)RB";
        d.name = R"RB(Dune Devourer)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-125/167)RB";
        d.collector_number = 125;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 7;
        d.power_cost = 1;
        d.might = 7;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Deflect);
        d.deflect_value = 1;
        d.ability_text = R"RB(You may kill a friendly unit as an additional cost to play me. If you do, I cost [2] less.
[Deflect] (Opponents must pay [A] to choose me with a spell or ability.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/4f49bf38f0d3abb71e148783ebec97f446db8166-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1050(CardRegistry& r) {
    r.registerCard(1050, std::make_unique<DuneDevourer>());
}

}  // namespace riftbound
