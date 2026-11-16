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

class IndustriousMechsmith : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, play a 3 [M] Mech unit token here."
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        createMech(ctx, locationHere(ctx.state, ctx.source, ctx.controller));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 993;
        d.def_id = R"RB(rad-049-167)RB";
        d.name = R"RB(Industrious Mechsmith)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-049/167)RB";
        d.collector_number = 49;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Mind};
        d.energy_cost = 4;
        d.might = 1;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play me, play a 3 [M] Mech unit token here.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/c8f6b4191874f23b522566f8ebeeb6e5ee18eb7f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_993(CardRegistry& r) {
    r.registerCard(993, std::make_unique<IndustriousMechsmith>());
}

}  // namespace riftbound
