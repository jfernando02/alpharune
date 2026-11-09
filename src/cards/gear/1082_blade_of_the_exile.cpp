#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include "cards/gear/equip_base.h"
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

// "[Equip] — [A] or exhaust a friendly unit."
// [A] is the usual equip (UniversalEquipGear); the engine also offers the equip
// paid by exhausting a ready friendly unit (Card::equipByExhaustingAUnit) — the
// unit equipped may be the one exhausted.
class BladeOfTheExile : public UniversalEquipGear {
public:
    BladeOfTheExile() : UniversalEquipGear(0) {}
    const CardDef& def() const override { return def_; }
    bool equipByExhaustingAUnit() const override { return true; }
    bool onEquipPaid(CardContext& ctx, GameObjectId unit) override {
        if (!ctx.state.objectExists(ctx.source) || !ctx.state.objectExists(unit)) return false;
        const auto& u = ctx.state.getObject(unit);
        if (!u.isUnit() || u.controller != ctx.controller || !u.location.has_value()) return false;
        attachEquipment(ctx, ctx.source, unit);
        return true;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1082;
        d.def_id = R"RB(rad-150-167)RB";
        d.name = R"RB(Blade of the Exile)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-150/167)RB";
        d.collector_number = 150;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Gear;
        d.super_type = SuperType::Signature;
        d.domains = {Domain::Calm, Domain::Chaos};
        d.tags = {R"RB(Equipment)RB", R"RB(Riven)RB"};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.might_bonus = 3;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Equip);
        d.ability_text = R"RB([Equip] — [A] or exhaust a friendly unit. (Pay the cost: Attach this to a unit you control.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/04aaded01b23641e496e39f51882ccfe89715492-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1082(CardRegistry& r) {
    r.registerCard(1082, std::make_unique<BladeOfTheExile>());
}

} // namespace riftbound
