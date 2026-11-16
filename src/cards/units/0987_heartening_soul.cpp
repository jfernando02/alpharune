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

class HearteningSoul : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, ready a friendly legend. Friendly legends have
    // "[E]: Give a unit +3 [M] this turn.""
    unsigned choicesAtFinalization() const override {
        return kFinalizeTrigger | kFinalizeActivate;
    }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        std::vector<GameObjectId> legends;
        for (auto l : ctx.state.player(ctx.controller).legends())
            if (ctx.state.objectExists(l)) legends.push_back(l);
        const auto t = pickTarget(ctx, "Heartening Soul: ready a friendly legend", legends);
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t)) ctx.executor.readyObject(t);
    }
    // The ability friendly legends have: "[E]: Give a unit +3 [M] this turn."
    std::vector<ActivatedAbility> activatedAbilities() const override {
        ActivatedAbility a;
        a.cost.exhaust = true;
        a.granted_only = true;
        a.needs_activation_time_target = true;
        return {a};
    }
    void onActivate(CardContext& ctx, int /*ability_index*/,
                    const std::vector<GameObjectId>& /*targets*/) override {
        std::vector<GameObjectId> units;
        for (const auto& [id, o] : ctx.state.objects)
            if (o.isUnit() && o.location.has_value()) units.push_back(id);
        const auto t = pickTarget(ctx, "Heartening Soul: give a unit +3 [M] this turn", units);
        if (finalizingChoices(ctx)) return;
        if (t == kInvalidId || !ctx.state.objectExists(t)) return;
        ctx.executor.giveTemporaryMight(t, 3);
    }
    void applyPassiveAura(GameState& state, PlayerId controller) const override {
        if (controller == PlayerId::None) return;
        for (auto l : state.player(controller).legends()) {
            if (!state.objectExists(l)) continue;
            auto& g = state.getObject(l).granted_abilities;
            bool has = false;
            for (const auto& r : g) has = has || r.source_def_id == cardDefId();
            if (!has) g.push_back({cardDefId(), 0});
        }
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 987;
        d.def_id = R"RB(rad-040-167)RB";
        d.name = R"RB(Heartening Soul)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-040/167)RB";
        d.collector_number = 40;
        d.artist = R"RB(Dao Trong Le)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Calm};
        d.energy_cost = 7;
        d.power_cost = 1;
        d.might = 7;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When you play me, ready a friendly legend.
Friendly legends have "[E]: Give a unit +3 [M] this turn.")RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0a5f9ab3216462356f6ddf8f172284dca6be21e9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_987(CardRegistry& r) {
    r.registerCard(987, std::make_unique<HearteningSoul>());
}

}  // namespace riftbound
