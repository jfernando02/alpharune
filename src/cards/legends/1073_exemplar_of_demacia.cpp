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

class ExemplarOfDemacia : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a gear with [Deploy], you may exhaust me and pay [A] to move
    // a friendly unit to that battlefield."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayAGear; }
    bool triggerCondition(const GameState& s, GameObjectId self, PlayerId, TriggerType, GameObjectId subject) const override {
        return s.objectExists(self) && subject != kInvalidId && s.objectExists(subject) && s.getObject(subject).deploy;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto g = triggerSubject(ctx);
        const auto bf = (g != kInvalidId && ctx.state.objectExists(g)) ? ctx.state.getObject(g).battlefieldId() : std::nullopt;
        const auto movers = [&] {
            std::vector<GameObjectId> out;
            for (auto u : friendlyUnits(ctx.state, ctx.controller))
                if (ctx.state.getObject(u).battlefieldId() != bf) out.push_back(u);
            return out;
        };
        const int go = confirmOptional(ctx, "Exemplar of Demacia: exhaust and pay [A] to move a unit there", [&] {
            if (costAlreadyPaid(ctx)) return true;
            return bf.has_value() && !ctx.state.getObject(ctx.source).is_exhausted &&
                   payableOnePower(ctx.state, ctx.controller, std::nullopt) && !movers().empty(); });
        if (go == -1) return;
        if (go == 0) { if (finalizingChoices(ctx)) return; return; }
        const auto u = pickTarget(ctx, "Exemplar of Demacia: move a friendly unit", movers());
        if (ctx.executor.hasPendingChoice()) return;
        bool paid = false;
        if (costDueNow(ctx)) {
            ctx.executor.exhaustObject(ctx.source);
            paid = payOnePower(ctx, ctx.controller, std::nullopt);
        }
        if (finalizingChoices(ctx)) return;
        if (!(paid || costAlreadyPaid(ctx)) || !bf || u == kInvalidId || !ctx.state.objectExists(u)) return;
        if (ctx.state.getObject(u).location) ctx.executor.moveToBattlefield(u, *bf);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1073;
        d.def_id = R"RB(rad-155-167)RB";
        d.name = R"RB(Exemplar of Demacia)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-155/167)RB";
        d.collector_number = 155;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Body, Domain::Order};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play a gear with [Deploy], you may exhaust me and pay [A] to move a friendly unit to that battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/e85c005f938e44655f926bb406a47359ae51ecc1-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1073(CardRegistry& r) {
    r.registerCard(1073, std::make_unique<ExemplarOfDemacia>());
}

}  // namespace riftbound
