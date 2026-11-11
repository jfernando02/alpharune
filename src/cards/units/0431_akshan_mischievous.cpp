#include "cards/card.h"
#include "cards/card_registry.h"
#include "cards/gear/equip_base.h"
#include "cards/units/weaponmaster_base.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class AkshanMischievous : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "You may pay [O][O] as an additional cost to play me." Now a real
    // optional play-time cost; the gear-steal below only happens "if you paid".
    OptionalAdditionalCost optionalAdditionalCost() const override {
        return {/*valid=*/true, /*energy=*/0, /*power=*/2, Domain::Order,
                /*any_domain=*/false, /*paid_flag=*/"__akshan_paid"};
    }
    // Two "when you play me" abilities ([Weaponmaster], CR 821, and the gear steal) share one
    // chain item: the trigger manager queues one item per trigger type per card. The steal
    // resolves first, then the Weaponmaster equip. All choices are made at finalization.
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        const PlayerId me = ctx.controller;
        // "if you paid the additional cost" -- the flag is set at play time.
        const bool paid = ctx.state.getObject(ctx.source).card_counters["__akshan_paid"] == 1;

        // "move an enemy gear to your base": the controller chooses which (pickTargetPair's
        // first pick; it has no second pick, so it does not collide with pickTarget below).
        GameObjectId target_gear = kInvalidId;
        if (paid) {
            std::vector<GameObjectId> enemy_gear;
            for (const auto& [id, obj] : ctx.state.objects)
                if (obj.isGear() && obj.controller != me && obj.location.has_value())
                    enemy_gear.push_back(id);
            std::sort(enemy_gear.begin(), enemy_gear.end());
            target_gear = pickTargetPair(ctx, "Akshan: which enemy gear to move to your base?",
                                         enemy_gear,
                                         [](GameObjectId) { return std::vector<GameObjectId>{}; })
                              .first;
            if (ctx.executor.hasPendingChoice()) return;
        }
        const GameObjectId equipment = chooseWeaponmasterEquipment(*this, ctx, def().name);
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)

        if (target_gear != kInvalidId && ctx.state.objectExists(target_gear) &&
            ctx.state.getObject(target_gear).location.has_value()) {
            if (ctx.state.getObject(target_gear).attached_to.has_value())
                ctx.executor.unattachGear(target_gear);
            // "You control it until I leave the board." Reverts to the original owner via
            // GameEngine::revertLapsedControl once this Akshan is gone.
            ctx.executor.takeControlUntilSourceLeaves(target_gear, me, ctx.source);
            ctx.executor.moveToBase(target_gear);
            ctx.events.logTrace("AKSHAN: stole enemy gear until I leave the board");
            const auto& g = ctx.state.getObject(target_gear);
            const bool is_equipment = std::find(g.tags.begin(), g.tags.end(), "Equipment") != g.tags.end();
            const auto& self = ctx.state.getObject(ctx.source);
            if (is_equipment && self.location.has_value()) {
                attachEquipment(ctx, target_gear, ctx.source);
                ctx.state.getObject(target_gear).controller = me;
                ctx.events.logTrace("AKSHAN: attached stolen Equipment to me");
            }
        }
        applyWeaponmaster(ctx, equipment, def().name);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 431;
        d.def_id = R"RB(sfd-109-221)RB";
        d.name = R"RB(Akshan, Mischievous)RB";
        d.set_code = R"RB(SFD)RB";
        d.set_name = R"RB(Spiritforged)RB";
        d.public_code = R"RB(SFD-109/221)RB";
        d.collector_number = 109;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Body};
        d.tags = {R"RB(Akshan)RB", R"RB(Shurima)RB", R"RB(Sentinel)RB"};
        d.energy_cost = 4;
        d.might = 4;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Weaponmaster);
        d.ability_text = R"RB([Weaponmaster]
You may pay [O][O] as an additional cost to play me.
When you play me, if you paid the additional cost, move an enemy gear to your base. You control it until I leave the board. If it's an Equipment, attach it to me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/f9e79f88463c1d516b9f1b053661937676e0e1f4-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_431(CardRegistry& r) {
    r.registerCard(431, std::make_unique<AkshanMischievous>());
}

} // namespace riftbound
