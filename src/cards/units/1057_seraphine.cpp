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

class SeraphineStarOfPiltover : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me or when I become exhausted, play a 1 [M] Recruit unit
    // token to your base. I have +1 [M] for each other exhausted unit you control."
    std::vector<TriggerType> triggerTypes() const override {
        return {TriggerType::WhenYouPlayMe, TriggerType::WhenIBecomeExhausted};
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        createRecruitAtBase(ctx);
    }
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        forEachSelfOnce(*this, state, controller, [&](GameObjectId sid, GameObject& self) {
            int n = 0;
            for (const auto& [id, o] : state.objects)
                if (id != sid && o.isUnit() && o.controller == controller && o.location.has_value() && o.is_exhausted) ++n;
            if (n > 0) auraMight(self, sid, n);
        });
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1057;
        d.def_id = R"RB(rad-138-167)RB";
        d.name = R"RB(Seraphine, Not Alone)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-138/167)RB";
        d.collector_number = 138;
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Order};
        d.energy_cost = 5;
        d.power_cost = 1;
        d.might = 1;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(When you play me or when I become exhausted, play a 1 [M] Recruit unit token to your base.
I have +1 [M] for each other exhausted unit you control.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/756f0d5d9e83ba0ebcfaf1b69055fdc29458b084-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1057(CardRegistry& r) {
    r.registerCard(1057, std::make_unique<SeraphineStarOfPiltover>());
}

}  // namespace riftbound
