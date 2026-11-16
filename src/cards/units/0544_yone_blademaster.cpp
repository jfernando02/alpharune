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

#include "cards/units/weaponmaster_base.h"

namespace riftbound {
namespace {

class YoneBlademaster : public UnitCard {
public:
    const CardDef& def() const override { return def_; }

    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayMe, TriggerType::WhenIConquer};
    }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }

    // "When I conquer a battlefield that was uncontrolled":
    // BattlefieldState::conquered_from_uncontrolled, set as control changes.
    bool triggerCondition(const GameState& state, GameObjectId source, PlayerId,
                          TriggerType trigger, GameObjectId) const override {
        if (trigger != TriggerType::WhenIConquer) return true;
        if (!state.objectExists(source)) return false;
        const auto bf = state.getObject(source).battlefieldId();
        if (!bf) return false;
        for (const auto& b : state.battlefields)
            if (b.id == *bf) return b.conquered_from_uncontrolled;
        return false;
    }

    void onTrigger(CardContext& ctx,
                   const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger == TriggerType::WhenYouPlayMe) {
            resolveWeaponmaster(*this, ctx, def().name);
            return;
        }
        if (ctx.firing_trigger != TriggerType::WhenIConquer) return;
        // "deal damage equal to my Might to an enemy unit in a base": chosen
        // as the trigger is finalized; my Might is read as it resolves.
        std::vector<GameObjectId> enemies;
        for (const auto& [id, obj] : ctx.state.objects)
            if (obj.isUnit() && obj.controller != ctx.controller && obj.isAtBase())
                enemies.push_back(id);
        GameObjectId unit = kInvalidId;
        if (!enemies.empty()) unit = pickTarget(ctx, "Yone: deal my Might to an enemy unit in a base", enemies);
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (unit == kInvalidId || !ctx.state.objectExists(unit) ||
            !ctx.state.getObject(unit).isAtBase() || !ctx.state.objectExists(ctx.source)) return;
        const int might = ctx.state.getObject(ctx.source).current_might;
        if (might <= 0) return;
        ctx.executor.dealDamage(unit, might, ctx.source);
        ctx.events.logTrace("YONE: conquered an uncontrolled battlefield -> dealt " +
                            std::to_string(might) + " to an enemy unit in a base");
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 544;
        d.def_id = R"RB(sfd-233-221)RB";
        d.name = R"RB(Yone, Blademaster)RB";
        d.set_code = R"RB(SFD)RB";
        d.set_name = R"RB(Spiritforged)RB";
        d.public_code = R"RB(SFD-233/221)RB";
        d.collector_number = 233;
        d.artist = R"RB(Anson Tan)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Body};
        d.tags = {R"RB(Yone)RB", R"RB(Ionia)RB", R"RB(Demon)RB"};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 5;
        d.rarity = Rarity::Showcase;
        d.keywords.set(Keyword::Weaponmaster);
        d.ability_text = R"RB([Weaponmaster] (When you play me, you may [Equip] one of your Equipment to me for [A] less, even if it's already attached.)
When I conquer a battlefield that was uncontrolled, deal damage equal to my Might to an enemy unit in a base.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0bb6fd6e7d1e6fefeeccc82059bdae2eaa000730-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_544(CardRegistry& r) {
    r.registerCard(544, std::make_unique<YoneBlademaster>());
}

} // namespace riftbound
