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

class DeathFromBelow : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true,
                                   .must_be_at_battlefield = true};
    }
    // The target is chosen as the spell goes on the chain, so the copy played
    // again from the trash chooses its own.
    bool needsPlayTimeTarget() const override { return true; }

    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& targets) override {
        auto& ri = ctx.state.chain.resuming;
        // First entry: kill. Re-entry after the "you may" answer skips straight to it.
        if (!ri.has_value() || ri->resume_point == 0) {
            if (targets.empty() || !ctx.state.objectExists(targets[0])) return;
            const int might = ctx.state.getObject(targets[0]).current_might;
            ctx.executor.killObject(targets[0]);
            ctx.events.logTrace("DEATH FROM BELOW: killed unit (might=" +
                                 std::to_string(might) + ")");
            if (might > 3) return;
        }
        // "You may play this from your trash for [A]": right away, as part of this
        // resolution (user ruling 2026-11-14), not a permission for later. It goes
        // on the chain again and chooses a new target.
        const PlayerId me = ctx.controller;
        const auto purpose = ctx.state.purposePlaying(CardType::Spell);
        const int go = confirmOptional(ctx, "play Death from Below again for [A]", [&] {
            return ctx.executor.payableAnyPower(me, purpose) >= 1 &&
                   !enumerateLegalTargets(ctx.state, me).empty();
        });
        if (go <= 0) return;
        if (!ctx.executor.payAnyPower(me, 1, purpose)) return;
        ctx.events.logTrace("DEATH FROM BELOW: played again from the trash for [A]");
        ctx.executor.playIgnoringCost(me, ctx.source);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 747;
        d.def_id = R"RB(unl-186-219)RB";
        d.name = R"RB(Death from Below)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-186/219)RB";
        d.collector_number = 186;
        d.artist = R"RB(莺之歌)RB";
        d.card_type = CardType::Spell;
        d.super_type = SuperType::Signature;
        d.domains = {Domain::Fury, Domain::Chaos};
        d.tags = {R"RB(Pyke)RB"};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Kill a unit at a battlefield. Then, if it had 3 [M] or less, do this: You may play this from your trash for [A].)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/17d93a11a252287e3b0f4bbe32722ecc9469ec66-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_747(CardRegistry& r) {
    r.registerCard(747, std::make_unique<DeathFromBelow>());
}

} // namespace riftbound
