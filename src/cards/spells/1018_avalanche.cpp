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

class Avalanche : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Deal 3 to all units in bases."
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        std::vector<GameObjectId> hit;
        for (const auto& [id, o] : ctx.state.objects)
            if (o.isUnit() && o.isAtBase()) hit.push_back(id);
        for (auto u : hit) ctx.executor.dealDamage(u, 3, ctx.source);
        killLethal(ctx, hit);
        ctx.events.logTrace("AVALANCHE: 3 to " + std::to_string(hit.size()) + " unit(s) in bases");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1018;
        d.def_id = R"RB(rad-079-167)RB";
        d.name = R"RB(Avalanche)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-079/167)RB";
        d.collector_number = 79;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Body};
        d.energy_cost = 5;
        d.power_cost = 2;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(Deal 3 to all units in bases.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/7b2103352935061e19cf90036a6aa4cd35ad5922-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1018(CardRegistry& r) {
    r.registerCard(1018, std::make_unique<Avalanche>());
}

}  // namespace riftbound
