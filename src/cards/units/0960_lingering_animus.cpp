#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

// "I enter ready if a friendly unit or gear died this turn."
// Reads PlayerState::unit_or_gear_died_this_turn (set when a unit or gear its
// player controlled dies, reset each turn).
class LingeringAnimus : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    bool entersReadyOnPlay(const GameState& state, PlayerId controller) const override {
        return state.player(controller).unit_or_gear_died_this_turn;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 960;
        d.def_id = R"RB(rad-005-167)RB";
        d.name = R"RB(Lingering Animus)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-005/167)RB";
        d.collector_number = 5;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Fury};
        d.energy_cost = 2;
        d.might = 2;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(I enter ready if a friendly unit or gear died this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5453008d27b3bb4f00941cb5a0d9ca52e4eb0245-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_960(CardRegistry& r) {
    r.registerCard(960, std::make_unique<LingeringAnimus>());
}

} // namespace riftbound
