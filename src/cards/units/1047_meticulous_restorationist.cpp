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

class MeticulousRestorationist : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a card with Energy cost [7] or more, you may play me from
    // your trash for [Y]."
    // APPROX: the play-from-trash ability is not modeled.
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1047;
        d.def_id = R"RB(rad-120-167)RB";
        d.name = R"RB(Meticulous Restorationist)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-120/167)RB";
        d.collector_number = 120;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 2;
        d.might = 2;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play a card with Energy cost [7] or more, you may play me from your trash for [Y].)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ee50fbf5e902e7109a3d54b417edc440b5a37198-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1047(CardRegistry& r) {
    r.registerCard(1047, std::make_unique<MeticulousRestorationist>());
}

}  // namespace riftbound
