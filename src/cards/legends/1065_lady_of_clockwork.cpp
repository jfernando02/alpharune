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

class LadyOfClockwork : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "At the start of your Main Phase, you may pay [1] to choose a friendly
    // gear. It becomes a unit gear with Might equal to its Energy cost until you
    // choose another gear with me."
    // APPROX: unit gear are not modeled; this legend has no effect.
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1065;
        d.def_id = R"RB(rad-145-167)RB";
        d.name = R"RB(Lady of Clockwork)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-145/167)RB";
        d.collector_number = 145;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Calm, Domain::Mind};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(At the start of your Main Phase, you may pay [1] to choose a friendly gear. It becomes a unit gear with Might equal to its Energy cost until you choose another gear with me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/d1f940425d4d913d8f56c017254343bc898f4892-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1065(CardRegistry& r) {
    r.registerCard(1065, std::make_unique<LadyOfClockwork>());
}

}  // namespace riftbound
