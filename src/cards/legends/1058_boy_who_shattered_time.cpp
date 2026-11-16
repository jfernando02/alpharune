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

class BoyWhoShatteredTime : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Empower] — [1], [E]. Disempower me, [E]: Reduce the cost of the next unit
    // you play this turn by [2][A] and give it "Kill me at end of turn.""
    EmpowerSpec empowerSpec() const override {
        EmpowerSpec s; s.valid = true; s.cost.energy = 1; s.cost.exhaust = true; return s;
    }
    std::vector<ActivatedAbility> activatedAbilities() const override {
        return {{.cost = {.exhaust = true, .disempower_self = true}}, empowerAbility()};
    }
    void onActivate(CardContext& ctx, int ability_index, const std::vector<GameObjectId>& /*targets*/) override {
        if (ability_index != 0) return;
        PlayerState::CostModifier m;
        m.source = ctx.source;
        m.energy_reduction = 2;
        m.power_reduction = 1;
        m.next_unit_only = true;
        m.units_only = true;
        m.affects_friendly_only = true;
        ctx.state.player(ctx.controller).cost_modifiers.push_back(m);
        if (ctx.state.objectExists(ctx.source))
            ctx.state.getObject(ctx.source).card_counters["__armed"] = ctx.state.turn.turn_number;
    }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayAUnit; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId, TriggerType, GameObjectId) const override {
        if (!s.objectExists(self)) return false;
        const auto& c = s.getObject(self).card_counters;
        const auto it = c.find("__armed");
        return it != c.end() && it->second == s.turn.turn_number;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        auto& me = ctx.state.getObject(ctx.source);
        if (ctx.firing_trigger == TriggerType::WhenYouPlayAUnit) {
            const auto u = triggerSubject(ctx);
            me.card_counters.erase("__armed");
            if (u == kInvalidId) return;
            me.card_counters["__eot_kill"] = static_cast<int>(u);
            DelayedAbility da;
            da.source = ctx.source;
            da.card_def_id = cardDefId();
            da.controller = ctx.controller;
            da.trigger = TriggerType::AtEndOfTurn;
            da.expires_on_turn = ctx.state.turn.turn_number;
            ctx.state.delayed_abilities.push_back(da);
            return;
        }
        const auto it = me.card_counters.find("__eot_kill");
        if (it == me.card_counters.end()) return;
        const auto u = static_cast<GameObjectId>(it->second);
        me.card_counters.erase(it);
        if (ctx.state.objectExists(u) && ctx.state.getObject(u).location) ctx.executor.killObject(u);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1058;
        d.def_id = R"RB(rad-139-167)RB";
        d.name = R"RB(Boy Who Shattered Time)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-139/167)RB";
        d.collector_number = 139;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Fury, Domain::Mind};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB([Empower] —  [1], [E] (Use only if not Empowered.)
Disempower me, [E]: Reduce the cost of the next unit you play this turn by [2][A] and give it "Kill me at end of turn.")RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/8ca3ef446631784ce1d261e30f6a163843ffcb2b-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1058(CardRegistry& r) {
    r.registerCard(1058, std::make_unique<BoyWhoShatteredTime>());
}

}  // namespace riftbound
