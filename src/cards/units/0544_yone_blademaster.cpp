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

    void onTrigger(CardContext& ctx,
                   const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.firing_trigger == TriggerType::WhenYouPlayMe) {
            resolveWeaponmaster(*this, ctx, def().name);
        } else if (ctx.firing_trigger == TriggerType::WhenIConquer) {
            if (finalizingChoices(ctx)) return;   // no choices
            doConquerDamage(ctx);
        }
    }

private:

    void doConquerDamage(CardContext& ctx) {
        // ENGINE GAP: printed condition is "When I conquer a battlefield THAT
        // WAS UNCONTROLLED". By the time WhenIConquer resolves, the BF is
        // already controlled by us and the prior-controller is not surfaced to
        // the trigger (no captured "was uncontrolled" flag), so we cannot gate
        // on it. Fires on any conquer (over-fires when conquering a BF that
        // was enemy-controlled). Low severity; documented approximation.
        if (!ctx.state.objectExists(ctx.source)) return;
        const auto& self = ctx.state.getObject(ctx.source);
        int my_might = self.current_might;
        if (my_might <= 0) return;
        PlayerId opp = opponent(ctx.controller);
        // Target an enemy unit in a base.
        for (auto& [id, obj] : ctx.state.objects) {
            if (!obj.isUnit() || obj.controller != opp) continue;
            if (!obj.isAtBase()) continue;
            ctx.executor.dealDamage(id, my_might, ctx.source);
            if (ctx.state.objectExists(id) &&
                ctx.state.getObject(id).hasLethalDamage()) {
                ctx.executor.killObject(id);
            }
            ctx.events.logTrace("YONE: conquer -> deal " +
                                 std::to_string(my_might) +
                                 " to enemy unit in base");
            return;
        }
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
        d.keywords.set(Keyword::Equip);
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
