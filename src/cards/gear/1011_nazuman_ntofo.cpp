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
#include "cards/gear/equip_base.h"

namespace riftbound {
namespace {

class NazumanNtofo : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // Equipment: "[Shield 2]" for the unit. "Equip [1]".
    bool hasEquipAbility() const override { return true; }
    bool canEquip(const GameState& s, PlayerId p) const override { return canPayEquipCost(s, p, 1, std::nullopt, 0); }
    bool onEquip(CardContext& ctx, GameObjectId unit) override {
        return ctx.state.objectExists(unit) && weaponmasterPayEnergyAndAttach(ctx, unit, 1, false);
    }

    int equippedShield() const override { return 2; }
    KeywordSet equippedKeywords() const override { KeywordSet k; k.set(Keyword::Shield); return k; }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1011;
        d.def_id = R"RB(rad-071-167)RB";
        d.name = R"RB(Nazuman Ntofo)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-071/167)RB";
        d.collector_number = 71;
        d.card_type = CardType::Gear;
        d.domains = {Domain::Body};  // the gallery says Order (a mislabel): its collector number is in the Body block, as riftdecks lists it
        d.tags = {R"RB(Equipment)RB"};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(Equip [1] ([1]: Attach this to a unit you control.) The equipped unit has [Shield 2].)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9d518a6db3ae174096b09cf80cece2997f685ede-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1011(CardRegistry& r) {
    r.registerCard(1011, std::make_unique<NazumanNtofo>());
}

}  // namespace riftbound
