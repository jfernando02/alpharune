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

class MeticulousRestorationist : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play a card with Energy cost [7] or more, you may play me from
    // your trash for [Y]."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayACardInTrash; }
    bool triggerCondition(const GameState& state, GameObjectId /*source*/, PlayerId /*controller*/,
                          TriggerType /*trigger*/, GameObjectId /*subject*/) const override {
        return state.turn.event_card_energy >= 7;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto purpose = ctx.state.purposePlaying(CardType::Unit);
        auto in_trash = [&] {
            return ctx.state.objectExists(ctx.source) &&
                   ctx.state.getObject(ctx.source).zone == ZoneType::Trash;
        };
        auto still_legal = [&] {
            return in_trash() && payableOnePower(ctx.state, ctx.controller, Domain::Order, purpose);
        };
        if (!costAlreadyPaid(ctx) && !still_legal()) return;
        const int conf = confirmOptional(ctx, "Meticulous Restorationist: play me from your trash for [Y]?",
                                         still_legal);
        if (conf < 1) return;
        // Where it is played (CR 355.2.a): 0 = base, 1 + i = battlefield i you control.
        const int n = 1 + static_cast<int>(ctx.state.battlefields.size());
        uint32_t legal = 1u;
        std::vector<std::string> labels = {"Base"};
        for (const auto& bf : ctx.state.battlefields) {
            labels.push_back("Battlefield #" + std::to_string(bf.id));
            if (bf.controller.has_value() && *bf.controller == ctx.controller)
                legal |= 1u << (1 + bf.id);
        }
        const int m = pickMode(ctx, "Meticulous Restorationist: play me where?", n, labels, legal);
        if (m == -1) return;
        if (costDueNow(ctx) && !payOnePower(ctx, ctx.controller, Domain::Order, purpose)) return;
        if (finalizingChoices(ctx)) return;   // choices and costs end here (CR 383.3)
        if (!in_trash()) return;
        LocationId where = BaseLocation{ctx.controller};
        if (m >= 1) {
            const auto bf = static_cast<BattlefieldId>(m - 1);
            for (const auto& b : ctx.state.battlefields)
                if (b.id == bf && b.controller.has_value() && *b.controller == ctx.controller)
                    where = BattlefieldLocation{bf};
        }
        pullFromZoneLists(ctx.state, ctx.source);
        ctx.executor.playIgnoringCost(ctx.controller, ctx.source, where);
        ctx.events.logTrace("METICULOUS RESTORATIONIST: played from trash for [Y]");
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1047;
        d.def_id = R"RB(rad-120-167)RB";
        d.name = R"RB(Meticulous Restorationist)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-120/167)RB";
        d.collector_number = 120;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 2;
        d.might = 2;
        d.rarity = Rarity::Common;
        d.ability_text = R"RB(When you play a card with Energy cost [7] or more, you may play me from your trash for [Y].)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/ee50fbf5e902e7109a3d54b417edc440b5a37198-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1047(CardRegistry& r) {
    r.registerCard(1047, std::make_unique<MeticulousRestorationist>());
}

}  // namespace riftbound
