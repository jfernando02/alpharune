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

class StarryEyedSongstress : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "As an additional cost to play a spell, you may exhaust me and three
    // friendly units to reduce the cost of that spell by [2]."
    // APPROX: not modeled; this legend has no effect.
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1069;
        d.def_id = R"RB(rad-151-167)RB";
        d.name = R"RB(Starry-Eyed Songstress)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-151/167)RB";
        d.collector_number = 151;
        d.artist = R"RB(Naifan Zhang)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Mind, Domain::Order};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(As an additional cost to play a spell, you may exhaust me and three friendly units to reduce the cost of that spell by [2].)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/399262205b15aa3b6306eb1d80a308791dcbe2ca-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1069(CardRegistry& r) {
    r.registerCard(1069, std::make_unique<StarryEyedSongstress>());
}

}  // namespace riftbound
