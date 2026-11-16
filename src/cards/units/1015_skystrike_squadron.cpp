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

class SkystrikeSquadron : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "You may return a friendly gear to its owner's hand as an additional cost to
    // play me. If you do, I cost [2] less. [Deflect]"
    OptionalAdditionalCost optionalAdditionalCost() const override {
        OptionalAdditionalCost c;
        c.valid = true;
        c.return_friendly_gear = true;
        c.reduce_energy = 2;
        c.paid_flag = "__skystrike_paid";
        return c;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1015;
        d.def_id = R"RB(rad-075-167)RB";
        d.name = R"RB(Skystrike Squadron)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-075/167)RB";
        d.collector_number = 75;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Body};
        d.energy_cost = 5;
        d.might = 4;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Deflect);
        d.deflect_value = 1;
        d.ability_text = R"RB(You may return a friendly gear to its owner's hand as an additional cost to play me. If you do, I cost [2] less.
[Deflect] (Opponents must pay [A] to choose me with a spell or ability.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/694a362579d3f2e9805cac0da4814ea911423033-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1015(CardRegistry& r) {
    r.registerCard(1015, std::make_unique<SkystrikeSquadron>());
}

}  // namespace riftbound
