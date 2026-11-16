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

class AmateurDemolitionist : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, play a Bomb gear token to a battlefield."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        playBombTrigger(*this, ctx, "AMATEUR DEMOLITIONIST");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 956;
        d.def_id = R"RB(rad-001-167)RB";
        d.name = R"RB(Amateur Demolitionist)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-001/167)RB";
        d.collector_number = 1;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};
        d.energy_cost = 2;
        d.might = 1;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play me, play a Bomb gear token to a battlefield. (It has [Deploy]. When it dies, deal 2 to an enemy there and kill your other Bombs there.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/3dca5e04ff8dff983993f87d5a82a7fd56d7bc9c-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_956(CardRegistry& r) {
    r.registerCard(956, std::make_unique<AmateurDemolitionist>());
}

}  // namespace riftbound
