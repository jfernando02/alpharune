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

class DancingGrenade : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Deal 2 to a unit. Its controller may play this spell again for [A]. If
    //  they do, this deals 1 additional Bonus Damage for each time this spell
    //  has dealt damage this turn."
    // The target is chosen as the spell is played (also when played again).
    // The unit's controller is asked with their own agent; paying [A] marks the
    // spell to go back on the chain under them as it leaves (ChainManager).
    bool needsPlayTimeTarget() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& targets) override {
        if (targets.empty() || !ctx.state.objectExists(ctx.source)) return;
        const GameObjectId t = targets[0];
        if (!ctx.state.objectExists(t) || !ctx.state.getObject(t).location.has_value()) return;
        auto& src = ctx.state.getObject(ctx.source);
        auto& c = src.card_counters;
        if (c["__dg_turn"] != ctx.state.turn.turn_number) {   // a new turn: count afresh
            c["__dg_turn"] = ctx.state.turn.turn_number;
            c["__dg_hits"] = 0;
        }
        const bool replayed = c["__dg_replayed"] != 0;
        c["__dg_replayed"] = 0;
        const int dmg = 2 + (replayed ? c["__dg_hits"] : 0);
        ctx.executor.dealDamage(t, dmg, ctx.source);
        c["__dg_hits"] += 1;
        ctx.events.logTrace("DANCING GRENADE: deal " + std::to_string(dmg));
        const PlayerId tc = ctx.state.getObject(t).controller;
        if (ctx.state.getObject(t).hasLethalDamage()) ctx.executor.killObject(t);
        if (tc == PlayerId::None || ctx.executor.payableAnyPower(tc) < 1) return;
        if (ctx.executor.chooseObjectOrNoneNow(tc, {ctx.source}) != ctx.source) return;
        if (!ctx.executor.payAnyPower(tc, 1)) return;
        auto& c2 = ctx.state.getObject(ctx.source).card_counters;
        c2["__dg_replayed"] = 1;
        c2["__replay_on_leave_chain_by"] = tc == PlayerId::Player1 ? 1 : 2;
        ctx.events.logTrace("DANCING GRENADE: its controller plays it again for [A]");
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true};
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 582;
        d.def_id = R"RB(unl-020-219)RB";
        d.name = R"RB(Dancing Grenade)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-020/219)RB";
        d.collector_number = 20;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(Deal 2 to a unit. Its controller may play this spell again for [A]. If they do, this deals 1 additional Bonus Damage for each time this spell has dealt damage this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/8afc30b1bc7cc1841dfe11cfa29adbcc02257845-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_582(CardRegistry& r) {
    r.registerCard(582, std::make_unique<DancingGrenade>());
}

} // namespace riftbound
