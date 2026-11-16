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

class HuntersCircle : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    // "For each player, the first unit or gear they play here on their turn costs [1] less."
    int playHereDiscount(const GameState& state, BattlefieldId bf, PlayerId player,
                         const GameObject& card) const override {
        if (!card.isUnit() && !card.isGear()) return 0;
        if (state.turn.turn_player != player) return 0;
        const auto& done = state.player(player).bfs_played_permanent_this_turn;
        return std::find(done.begin(), done.end(), bf) == done.end() ? 1 : 0;
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1083;
        d.def_id = R"RB(rad-163-167)RB";
        d.name = R"RB(Hunters' Circle)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-163/167)RB";
        d.collector_number = 163;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(For each player, the first unit or gear they play here on their turn costs [1] less.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/e7e25d90506d805f1996e7f751cb5b6baa8ec305-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1083(CardRegistry& r) {
    r.registerCard(1083, std::make_unique<HuntersCircle>());
}

}  // namespace riftbound
