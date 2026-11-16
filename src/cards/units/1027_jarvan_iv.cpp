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

class JarvanIVMartialParagon : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "Your gear with [Deploy] have [Action]. When I conquer, if there is a
    // friendly gear with [Deploy] here, channel 1 rune exhausted."
    // [Action] on the controller's [Deploy] gear (in hand: played in showdowns).
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        for (auto& [id, o] : state.objects)
            if (o.isGear() && o.deploy && o.controller == controller)
                o.aura_keywords.set(Keyword::Action);
    }

    TriggerType triggerType() const override { return TriggerType::WhenIConquer; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        const auto bf = ctx.state.getObject(ctx.source).battlefieldId();
        if (!bf) return;
        for (const auto& [id, o] : ctx.state.objects)
            if (o.isGear() && o.deploy && o.controller == ctx.controller && o.battlefieldId() == bf) {
                ctx.executor.channelRunes(ctx.controller, 1, /*enter_exhausted=*/true);
                return;
            }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1027;
        d.def_id = R"RB(rad-091-167)RB";
        d.name = R"RB(Jarvan IV, Martial Paragon)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-091/167)RB";
        d.collector_number = 91;
        d.artist = R"RB(JunHuan)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Body};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.might = 4;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Your gear with [Deploy] have [Action]. (They can be played on your turn or in showdowns.)
When I conquer, if there is a friendly gear with [Deploy] here, channel 1 rune exhausted.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/8edacba51db193506e13e1afb6a8509256cdf55c-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1027(CardRegistry& r) {
    r.registerCard(1027, std::make_unique<JarvanIVMartialParagon>());
}

}  // namespace riftbound
