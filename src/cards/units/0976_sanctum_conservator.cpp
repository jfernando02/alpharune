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

class SanctumConservator : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a card with Energy cost [7] or more, ready me."
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayACard; }
    bool triggerCondition(const GameState& s, GameObjectId, PlayerId, TriggerType, GameObjectId subject) const override {
        if (subject == kInvalidId || !s.objectExists(subject)) return false;
        return s.getObject(subject).card_def_id != kInvalidId;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto subj = triggerSubject(ctx);
        if (subj == kInvalidId || !ctx.state.objectExists(subj)) return;
        if (printedEnergy(ctx, ctx.state.getObject(subj)) < 7) return;
        ctx.executor.readyObject(ctx.source);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 976;
        d.def_id = R"RB(rad-030-167)RB";
        d.name = R"RB(Sanctum Conservator)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-030/167)RB";
        d.collector_number = 30;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 5;
        d.might = 5;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play a card with Energy cost [7] or more, ready me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/776973101613a15a7d0a2fc90587d19098ecc894-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_976(CardRegistry& r) {
    r.registerCard(976, std::make_unique<SanctumConservator>());
}

}  // namespace riftbound
