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

// "[Reaction] [Show Off] a gear. Give a unit +2 [M] this turn. If you showed
//  off, ready up to 2 runes."
// The unit is chosen when the spell is finalized.
// Id 1100: Battle Preparations is missing from the card gallery snapshot the
// Radiance ids were ranked from (956 + rank); 1100 is the one id left past them.
// APPROX: the engine has no [Show Off] step yet. A gear is shown off whenever
// one can be (a gear in hand or a friendly gear), checked as the spell
// resolves; the unit is a friendly one (+2 [M] to an enemy is never wanted).
class BattlePreparations : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    TargetRequirements getTargetRequirements() const override {
        return TargetRequirements{.count = 1, .must_be_unit = true, .must_be_friendly = true};
    }
    bool needsPlayTimeTarget() const override { return true; }
    static bool canShowOffAGear(const GameState& state, PlayerId me) {
        for (auto cid : state.player(me).hand)
            if (state.objectExists(cid) && state.getObject(cid).isGear()) return true;
        for (const auto& [id, o] : state.objects)
            if (o.isGear() && o.controller == me && o.location.has_value()) return true;
        return false;
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        GameObjectId unit = pickTarget(ctx, "Battle Preparations: give a unit +2 [M] this turn",
                                       enumerateLegalTargets(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (unit != kInvalidId && ctx.state.objectExists(unit) &&
            ctx.state.getObject(unit).location.has_value())
            ctx.executor.giveTemporaryMight(unit, 2);
        if (!canShowOffAGear(ctx.state, ctx.controller)) return;
        int readied = 0;
        for (auto& [id, o] : ctx.state.objects) {
            if (readied >= 2) break;
            if (!o.isRune() || o.controller != ctx.controller || !o.location.has_value()) continue;
            if (!o.is_exhausted) continue;
            ctx.executor.readyObject(id);
            ++readied;
        }
        ctx.events.logTrace("BATTLE PREPARATIONS: +2 [M]; showed off a gear -> readied " +
                            std::to_string(readied) + " runes");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1100;
        d.def_id = R"RB(rad-032-167)RB";
        d.name = R"RB(Battle Preparations)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-032/167)RB";
        d.collector_number = 32;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 2;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
[Show Off] a gear. (As you play this, you may reveal a gear from hand or pick a friendly gear.)
Give a unit +2 [M] this turn. If you showed off, ready up to 2 runes.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/9422caf2fcd4cfb346cf0dce48245ef13a77180f-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1100(CardRegistry& r) {
    r.registerCard(1100, std::make_unique<BattlePreparations>());
}

} // namespace riftbound
