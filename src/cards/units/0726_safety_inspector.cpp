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

// "You may spend 3 XP as an additional cost to play me.
//  When you play me, each player must kill one of their units. If you paid my
//  additional cost, you don't kill a unit this way."
//
// The optional 3-XP payment is offered at trigger time via confirmOptional.
// The controller's own kill is the controller's choice (pickTarget); the
// opponent chooses theirs with their own agent.

class SafetyInspector : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);

        // Optional additional cost: spend 3 XP (skips the controller's kill).
        int paid = confirmOptional(ctx, "Safety Inspector: spend 3 XP to skip your kill?",
            [&]() { return ctx.state.player(ctx.controller).xp >= 3; });
        if (paid == -1) return;  // waiting on agent

        bool skip_own_kill = false;
        if (paid == 1 && ps.xp >= 3) {
            ps.xp -= 3;
            skip_own_kill = true;
            ctx.events.logTrace("SAFETY INSPECTOR: paid 3 XP -> skip own kill");
        }

        // Controller's kill (agent-chosen) unless they paid.
        if (!skip_own_kill) {
            std::vector<GameObjectId> own;
            for (auto& [id, obj] : ctx.state.objects) {
                if (!obj.isUnit() || obj.controller != ctx.controller) continue;
                if (!obj.location.has_value()) continue;
                own.push_back(id);
            }
            if (!own.empty()) {
                GameObjectId victim = pickTarget(ctx, "Safety Inspector: kill one of your units",
                                                 own);
                if (victim == kInvalidId && ctx.state.chain.resuming.has_value() &&
                    ctx.state.chain.resuming->resume_point == 7) {
                    return;  // suspended for agent decision
                }
                if (victim != kInvalidId && ctx.state.objectExists(victim))
                    ctx.executor.killObject(victim);
            }
        }

        // The opponent's forced kill: they choose, with their own agent.
        PlayerId opp = opponent(ctx.controller);
        std::vector<GameObjectId> theirs;
        for (auto& [id, obj] : ctx.state.objects)
            if (obj.isUnit() && obj.controller == opp && obj.location.has_value()) theirs.push_back(id);
        if (theirs.empty()) return;
        const GameObjectId opp_victim = ctx.executor.chooseObjectNow(opp, theirs);
        if (opp_victim != kInvalidId && ctx.state.objectExists(opp_victim)) {
            ctx.executor.killObject(opp_victim);
            ctx.events.logTrace("SAFETY INSPECTOR: opponent's chosen unit killed");
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 726;
        d.def_id = R"RB(unl-164-219)RB";
        d.name = R"RB(Safety Inspector)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-164/219)RB";
        d.collector_number = 164;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.tags = {R"RB(Yordle)RB", R"RB(Bandle City)RB"};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 3;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(You may spend 3 XP as an additional cost to play me.
When you play me, each player must kill one of their units. If you paid my additional cost, you don't kill a unit this way.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/499e65eb2d35e2bd774ea7fe2a70672234f71e90-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_726(CardRegistry& r) {
    r.registerCard(726, std::make_unique<SafetyInspector>());
}

} // namespace riftbound
