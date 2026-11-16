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

class LadyOfClockwork : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "At the start of your Main Phase, you may pay [1] to choose a friendly
    // gear. It becomes a unit gear with Might equal to its Energy cost until you
    // choose another gear with me."
    // The chosen gear is kept on the legend (card_counters "__unit_gear" = id+1,
    // "__unit_gear_might"); applyPassiveAura makes it a unit gear each cleanup.
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::AtStartOfMain; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto gears = [&] {
            std::vector<GameObjectId> out;
            for (const auto& [id, o] : ctx.state.objects)
                if (o.isGear() && o.controller == ctx.controller && o.location.has_value())
                    out.push_back(id);
            return out;
        };
        auto still_legal = [&] {
            return payableEnergy(ctx.state, ctx.controller) >= 1 && !gears().empty();
        };
        if (!costAlreadyPaid(ctx) && !still_legal()) return;
        const int conf = confirmOptional(ctx, "Lady of Clockwork: pay [1] to make a gear a unit?",
                                         still_legal);
        if (conf < 1) return;
        const auto g = pickTarget(ctx, "Lady of Clockwork: choose a friendly gear", gears());
        if (costDueNow(ctx) && !payEnergy(ctx, ctx.controller, 1)) return;
        if (finalizingChoices(ctx)) return;   // choices and costs end here (CR 383.3)
        if (g == kInvalidId || !ctx.state.objectExists(g) || !ctx.state.objectExists(ctx.source))
            return;
        if (ctx.state.getObject(g).attached_to.has_value()) ctx.executor.unattachGear(g);
        auto& me = ctx.state.getObject(ctx.source);
        me.card_counters["__unit_gear"] = static_cast<int>(g) + 1;
        me.card_counters["__unit_gear_might"] = printedEnergy(ctx, ctx.state.getObject(g));
        ctx.events.logTrace("LADY OF CLOCKWORK: " + ctx.state.getObject(g).name +
                            " becomes a unit gear");
    }
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        if (controller == PlayerId::None) return;
        for (auto l : state.player(controller).legends()) {
            if (!state.objectExists(l) || state.getObject(l).card_def_id != cardDefId()) continue;
            const auto& c = state.getObject(l).card_counters;
            auto it = c.find("__unit_gear");
            if (it == c.end() || it->second <= 0) continue;
            const auto g = static_cast<GameObjectId>(it->second - 1);
            if (!state.objectExists(g)) continue;
            auto& go = state.getObject(g);
            if (!go.isGear() || go.controller != controller || !go.location.has_value() ||
                go.attached_to.has_value())
                continue;
            go.is_unit_gear = true;
            auto m = c.find("__unit_gear_might");
            go.aura_might_bonus += (m == c.end() ? 0 : m->second);
        }
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1065;
        d.def_id = R"RB(rad-145-167)RB";
        d.name = R"RB(Lady of Clockwork)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-145/167)RB";
        d.collector_number = 145;
        d.artist = R"RB(Envar Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Calm, Domain::Mind};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(At the start of your Main Phase, you may pay [1] to choose a friendly gear. It becomes a unit gear with Might equal to its Energy cost until you choose another gear with me.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/d1f940425d4d913d8f56c017254343bc898f4892-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1065(CardRegistry& r) {
    r.registerCard(1065, std::make_unique<LadyOfClockwork>());
}

}  // namespace riftbound
