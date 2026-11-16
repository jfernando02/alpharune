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

class Fanfare : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "The next three times you play a unit this turn, draw 1."
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.state.player(ctx.controller).draw_on_unit_play_this_turn += 3;
        ctx.events.logTrace("FANFARE: the next three units played this turn draw 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 991;
        d.def_id = R"RB(rad-047-167)RB";
        d.name = R"RB(Fanfare)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-047/167)RB";
        d.collector_number = 47;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind};
        d.energy_cost = 3;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(The next three times you play a unit this turn, draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/75a3d8fbfd8caeabfd5be38513a6624c61f5230f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_991(CardRegistry& r) {
    r.registerCard(991, std::make_unique<Fanfare>());
}

}  // namespace riftbound
