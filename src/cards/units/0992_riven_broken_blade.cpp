#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "When I become exhausted, choose one that hasn't been chosen this turn —
//  [Buff] me. / Spend my buff to [Stun] an enemy unit. / Spend my buff to ready me."
// The mode, the stun target and the buff spent (the cost) are settled when the
// trigger is finalized (CR 383.3). The modes chosen this turn are kept on the
// unit: __riven_turn = the turn they belong to, __riven_modes = a bit per mode.
class RivenBrokenBlade : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIBecomeExhausted; }

    static int chosenThisTurn(const GameState& state, const GameObject& self) {
        auto t = self.card_counters.find("__riven_turn");
        if (t == self.card_counters.end() || t->second != state.turn.turn_number) return 0;
        auto m = self.card_counters.find("__riven_modes");
        return m == self.card_counters.end() ? 0 : m->second;
    }
    static std::vector<GameObjectId> stunnable(const GameState& state, PlayerId me) {
        return chooseableUnits(state, me, [&](const GameObject& o) { return o.controller != me; });
    }

    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        auto& self = ctx.state.getObject(ctx.source);
        const int used = chosenThisTurn(ctx.state, self);
        const bool has_buff = self.buff_count > 0;
        uint32_t legal = 0;
        if (!(used & 1)) legal |= 1u;
        if (!(used & 2) && has_buff && !stunnable(ctx.state, ctx.controller).empty()) legal |= 2u;
        if (!(used & 4) && has_buff && self.is_exhausted) legal |= 4u;

        const int mode = pickMode(ctx, "Riven, Broken Blade", 3,
                                  {"buff me", "spend my buff: stun an enemy unit",
                                   "spend my buff: ready me"},
                                  legal);
        if (mode == -1) return;   // waiting for the agent
        GameObjectId stun = kInvalidId;
        if (mode == 1) {
            stun = pickTarget(ctx, "Riven, Broken Blade: stun an enemy unit",
                              stunnable(ctx.state, ctx.controller));
            if (ctx.executor.hasPendingChoice()) return;
        }
        // "Spend my buff" is the cost of the second and third modes.
        if ((mode == 1 || mode == 2) && costDueNow(ctx) && self.buff_count > 0) {
            ctx.executor.spendBuff(ctx.source);
        }
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (mode < 0) return;                 // no mode left this turn

        if (self.card_counters["__riven_turn"] != ctx.state.turn.turn_number) {
            self.card_counters["__riven_turn"] = ctx.state.turn.turn_number;
            self.card_counters["__riven_modes"] = 0;
        }
        self.card_counters["__riven_modes"] |= (1 << mode);

        if (mode == 0) {
            if (self.buff_count == 0) ctx.executor.buffUnit(ctx.source);
            ctx.events.logTrace("RIVEN, BROKEN BLADE: buff me");
        } else if (mode == 1) {
            if (stun != kInvalidId && ctx.state.objectExists(stun) &&
                ctx.state.getObject(stun).location.has_value())
                ctx.executor.stunUnitBy(stun, ctx.source);
            ctx.events.logTrace("RIVEN, BROKEN BLADE: spent my buff -> stun an enemy unit");
        } else {
            ctx.executor.readyObject(ctx.source);
            ctx.events.logTrace("RIVEN, BROKEN BLADE: spent my buff -> ready me");
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 992;
        d.def_id = R"RB(rad-043-167)RB";
        d.name = R"RB(Riven, Broken Blade)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-043/167)RB";
        d.collector_number = 43;
        d.artist = R"RB(JunHuan)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Calm};
        d.tags = {R"RB(Riven)RB"};
        d.energy_cost = 4;
        d.might = 4;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When I become exhausted, choose one that hasn't been chosen this turn —
- [Buff] me. (Give me a +1 [M] buff if I don't have one.)
- Spend my buff to [Stun] an enemy unit. (It doesn't deal combat damage this turn.)
- Spend my buff to ready me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0c9ac0da1c144f46a1e0c5423f51f9b8f6ba295b-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_992(CardRegistry& r) {
    r.registerCard(992, std::make_unique<RivenBrokenBlade>());
}

} // namespace riftbound
