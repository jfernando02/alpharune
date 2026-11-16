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

class PrimordialRoar : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Show Off] a unit. If you showed off a unit, deal damage equal to that
    // unit's Might to a unit." Shows off whenever it can.
    static std::vector<GameObjectId> showable(const GameState& s, PlayerId p, GameObjectId self) {
        std::vector<GameObjectId> out;
        for (const auto& [id, o] : s.objects)
            if (id != self && o.isUnit() && o.controller == p && o.location.has_value()) out.push_back(id);
        for (auto c : s.player(p).hand)
            if (c != self && s.objectExists(c) && s.getObject(c).isUnit()) out.push_back(c);
        return out;
    }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    bool needsPlayTimeTargetPair() const override { return true; }
    TargetRequirements getTargetRequirements() const override { return TargetRequirements{.count = 2}; }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !anyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [shown, target] = pickTargetPair(ctx, "Primordial Roar: show off a unit, then a unit to hit",
            showable(ctx.state, ctx.controller, ctx.source),
            [&](GameObjectId) { return anyUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (shown == kInvalidId || target == kInvalidId || !ctx.state.objectExists(shown)) return;
        const auto& s = ctx.state.getObject(shown);
        const int dmg = s.location.has_value() ? s.current_might : s.base_might;
        if (ctx.state.objectExists(target) && ctx.state.getObject(target).location) dealAndKill(ctx, target, dmg);
        ctx.events.logTrace("PRIMORDIAL ROAR: showed off " + s.name + ", dealt " + std::to_string(dmg));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1020;
        d.def_id = R"RB(rad-082-167)RB";
        d.name = R"RB(Primordial Roar)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-082/167)RB";
        d.collector_number = 82;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Body};
        d.energy_cost = 4;
        d.power_cost = 2;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB([Show Off] a unit. (As you play this, you may reveal a unit from hand or pick a friendly unit.)
If you showed off a unit, deal damage equal to that unit's Might to a unit.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/945633b0c2ed7f303245c917ffbcdb80dcfb524e-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1020(CardRegistry& r) {
    r.registerCard(1020, std::make_unique<PrimordialRoar>());
}

}  // namespace riftbound
