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

class GravesBlastingThrough : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When I attack or defend, deal damage equal to my Might to all enemy units
    // here. I don't deal combat damage."
    TriggerType triggerType() const override { return TriggerType::WhenIAttackOrDefend; }
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        forEachSelfOnce(*this, state, controller, [&](GameObjectId sid, GameObject& self) {
            GameObject::AuraEffect ae;
            ae.source = sid;
            ae.suppress_combat_damage = true;
            self.aura_effects.push_back(ae);
        });
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        const auto& me = ctx.state.getObject(ctx.source);
        const auto bf = me.battlefieldId();
        if (!bf) return;
        const int dmg = me.current_might;
        std::vector<GameObjectId> hit;
        for (const auto& [id, o] : ctx.state.objects)
            if (o.isUnit() && o.controller != ctx.controller && o.battlefieldId() == bf) hit.push_back(id);
        for (auto u : hit) ctx.executor.dealDamage(u, dmg, ctx.source);
        killLethal(ctx, hit);
        ctx.events.logTrace("GRAVES: " + std::to_string(dmg) + " to " + std::to_string(hit.size()) + " enemy unit(s)");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1023;
        d.def_id = R"RB(rad-085-167)RB";
        d.name = R"RB(Graves, Blasting Through)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-085/167)RB";
        d.collector_number = 85;
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Body};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 4;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When I attack or defend, deal damage equal to my Might to all enemy units here.
I don't deal combat damage.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/5bc0671f9f78630e2932052bb9f7f42ef2742c86-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1023(CardRegistry& r) {
    r.registerCard(1023, std::make_unique<GravesBlastingThrough>());
}

}  // namespace riftbound
