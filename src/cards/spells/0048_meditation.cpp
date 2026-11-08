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

class Meditation : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "As an additional cost to play this, you may exhaust a friendly unit. If
    // you do, draw 2. Otherwise, draw 1." The optional cost — whether, and
    // which unit — is decided and paid as the spell is played (CR 383.3).
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    static std::vector<GameObjectId> readyFriendlyUnits(const GameState& state, PlayerId controller) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (obj.isUnit() && obj.controller == controller && obj.location.has_value() &&
                !obj.is_exhausted)
                out.push_back(id);
        return out;
    }
    void onResolve(CardContext& ctx,
                   const std::vector<GameObjectId>& /*targets*/) override {
        const int go = confirmOptional(ctx,
            "Meditation: exhaust a friendly unit to draw 2 (else draw 1)?",
            [&]() { return !readyFriendlyUnits(ctx.state, ctx.controller).empty(); });
        if (go < 0) return;  // waiting for agent choice
        GameObjectId paid = kInvalidId;
        if (go == 1) {
            paid = pickTarget(ctx, "Meditation: exhaust a friendly unit",
                              costChoices(ctx, readyFriendlyUnits(ctx.state, ctx.controller)));
            if (ctx.executor.hasPendingChoice()) return;  // waiting for the pick
            if (paid != kInvalidId && ctx.state.objectExists(paid) && costDueNow(ctx))
                ctx.executor.exhaustObject(paid);
        }
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        const bool exhausted = go == 1 && (paid != kInvalidId || costAlreadyPaid(ctx));
        ctx.executor.drawCards(ctx.controller, exhausted ? 2 : 1);
        ctx.events.logTrace(exhausted ? "MEDITATION: exhausted a friendly unit -> draw 2"
                                      : "MEDITATION: draw 1");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 48;
        d.def_id = R"RB(ogn-048-298)RB";
        d.name = R"RB(Meditation)RB";
        d.set_code = R"RB(OGN)RB";
        d.set_name = R"RB(Origins)RB";
        d.public_code = R"RB(OGN-048/298)RB";
        d.collector_number = 48;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 2;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
As an additional cost to play this, you may exhaust a friendly unit. If you do, draw 2. Otherwise, draw 1.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/7103336ab0d7f7bebdaa4155e4258d8d9beb06de-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_48(CardRegistry& r) {
    r.registerCard(48, std::make_unique<Meditation>());
}

} // namespace riftbound
