#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class MageseekerInvestigator : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "Opponents must pay [A] for each unit beyond the first to move multiple
    //  units to my battlefield at the same time." Represented via
    //  BattlefieldState::multi_move_surcharge_power for the opponent, one [A]
    //  per Investigator there; the engine charges it as units move together
    //  (CR 144.3).
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        if (controller == PlayerId::None) return;
        for (auto& b : state.battlefields) {
            int n = 0;
            for (const auto& [id, obj] : state.objects)
                if (obj.card_def_id == cardDefId() && obj.controller == controller &&
                    obj.battlefieldId() == b.id) ++n;
            b.multi_move_surcharge_power[playerIndex(opponent(controller))] = n;
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 725;
        d.def_id = R"RB(unl-163-219)RB";
        d.name = R"RB(Mageseeker Investigator)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-163/219)RB";
        d.collector_number = 163;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.tags = {R"RB(Demacia)RB"};
        d.energy_cost = 4;
        d.might = 4;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(Opponents must pay [A] for each unit beyond the first to move multiple units to my battlefield at the same time.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/004c77c98bc5a6f679191a0d289b26ac13e84728-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_725(CardRegistry& r) {
    r.registerCard(725, std::make_unique<MageseekerInvestigator>());
}

} // namespace riftbound
