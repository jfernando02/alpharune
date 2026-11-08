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

// "As an additional cost to play this, exhaust a friendly unit. [Stun] a unit."
// The play is gated on having a ready friendly unit; the unit to exhaust and
// the unit to stun are chosen when the spell is finalized (CR 355), and the
// exhaust is paid then.
class KiBurst : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> readyFriendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value() &&
                !obj.is_exhausted)
                out.push_back(id);
        return out;
    }
    static std::vector<GameObjectId> stunnableUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects) {
            if (!obj.isUnit() || !obj.location.has_value()) continue;
            if (obj.controller != controller && obj.untargetable_by_enemy) continue;
            out.push_back(id);
        }
        return out;
    }
    bool canBePlayed(const GameState& state, PlayerId player) const override {
        return !readyFriendlyUnits(state, player).empty();
    }
    bool hasLegalTargets(const GameState& state, PlayerId controller) const override {
        return !readyFriendlyUnits(state, controller).empty();  // that unit can be stunned too
    }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 2, .must_be_unit = true};
    }
    bool needsPlayTimeTargetPair() const override { return true; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto [paid, stunned] = pickTargetPair(
            ctx, "Ki Burst: exhaust a friendly unit, then stun a unit",
            costChoices(ctx, readyFriendlyUnits(ctx.state, ctx.controller)),
            [&](GameObjectId) { return stunnableUnits(ctx.state, ctx.controller); });
        if (ctx.executor.hasPendingChoice()) return;  // waiting for a pick
        if (paid != kInvalidId && ctx.state.objectExists(paid) && costDueNow(ctx))
            ctx.executor.exhaustObject(paid);
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (paid == kInvalidId && !costAlreadyPaid(ctx)) return;  // the cost was never paid
        if (stunned == kInvalidId || !ctx.state.objectExists(stunned)) return;
        ctx.executor.stunUnitBy(stunned, ctx.source);
        ctx.events.logTrace("KI BURST: exhausted a friendly unit -> stun");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 980;
        d.def_id = R"RB(rad-028-167)RB";
        d.name = R"RB(Ki Burst)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-028/167)RB";
        d.collector_number = 28;
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
As an additional cost to play this, exhaust a friendly unit.
[Stun] a unit. (It doesn't deal combat damage this turn.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/33af916b50007c01b33e78aef3dc99b757edc3f2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_980(CardRegistry& r) {
    r.registerCard(980, std::make_unique<KiBurst>());
}

} // namespace riftbound
