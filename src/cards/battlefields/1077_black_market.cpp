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

class BlackMarket : public BattlefieldCard {
public:
    const CardDef& def() const override { return def_; }
    // "[A][A]: Draw 1. Each player may use this ability once on their turn (even
    // if they don't control this battlefield)."
    bool activatableByAnyPlayer() const override { return true; }
    std::vector<ActivatedAbility> activatedAbilities() const override {
        ActivatedAbility a;
        a.cost.power = 2;
        a.cost.power_any_domain = true;
        return {a};
    }
    // Once per player, on their own turn.
    bool canActivateAbilityAt(const GameState& state, PlayerId player,
                              GameObjectId source, int ai) const override {
        if (!Card::canActivateAbilityAt(state, player, source, ai)) return false;
        if (state.turn.turn_player != player) return false;
        const auto& counters = state.getObject(source).card_counters;
        auto it = counters.find("__ability_used_turn_p" +
                                std::to_string(static_cast<int>(player)) + "_" +
                                std::to_string(ai));
        return it == counters.end() || it->second != state.turn.turn_number + 1;
    }
    void onActivate(CardContext& ctx, int /*ability_index*/,
                    const std::vector<GameObjectId>& /*targets*/) override {
        ctx.executor.drawCards(ctx.controller, 1);
        ctx.events.logTrace("BLACK MARKET: draw 1");
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1077;
        d.def_id = R"RB(rad-158-167)RB";
        d.name = R"RB(Black Market)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-158/167)RB";
        d.collector_number = 158;
        d.artist = R"RB(Fish Art Studio)RB";
        d.card_type = CardType::Battlefield;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB([A][A]: Draw 1. Each player may use this ability once on their turn (even if they don't control this battlefield).)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9a02ae108f32a8b88be47a94b5f0d39b5a06a6a1-1039x744.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1077(CardRegistry& r) {
    r.registerCard(1077, std::make_unique<BlackMarket>());
}

}  // namespace riftbound
