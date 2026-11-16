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

// [Ambush] engine-handled.
// "As an additional cost to play me, kill a Bird, Cat, Dog, or Poro you
//  control. You may play me to its battlefield (even if you don't have other
//  units there)."
//
// The kill is a required additional cost (executePlayCard asks for it as I am
// played, before paying), restricted to Bird/Cat/Dog/Poro by
// additionalCostAccepts. "You may [Ambush] me to its battlefield": [Ambush]
// already allows a battlefield where the player has a unit as I am played,
// which the unit about to be killed is.

class StalkingWolf : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    static bool fodder(const GameObject& obj) {
        return obj.isUnit() && (hasTag(obj, "Bird") || hasTag(obj, "Cat") ||
                                hasTag(obj, "Dog") || hasTag(obj, "Poro"));
    }
    OptionalAdditionalCost optionalAdditionalCost() const override {
        OptionalAdditionalCost c;
        c.valid = true;
        c.kill_friendly_unit = true;
        c.required = true;
        return c;
    }
    bool additionalCostAccepts(const GameState&, const GameObject& o) const override {
        return fodder(o);
    }
    bool canBePlayed(const GameState& state, PlayerId player) const override {
        for (const auto& [id, obj] : state.objects)
            if (obj.controller == player && obj.location.has_value() && fodder(obj)) return true;
        return false;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 728;
        d.def_id = R"RB(unl-166-219)RB";
        d.name = R"RB(Stalking Wolf)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-166/219)RB";
        d.collector_number = 166;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.tags = {R"RB(Dog)RB", R"RB(Freljord)RB"};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.might = 6;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Ambush);
        d.ability_text = R"RB([Ambush] (You may play me as a [Reaction] to a battlefield where you have units.)
As an additional cost to play me, kill a Bird, Cat, Dog, or Poro you control. You may [Ambush] me to its battlefield, even if you don't have other units there.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5f5f66caad21d59bf966ddc501bb4f9c84a595c6-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_728(CardRegistry& r) {
    r.registerCard(728, std::make_unique<StalkingWolf>());
}

} // namespace riftbound
