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

class MournfulSpirit : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Deathknell][>] You may kill a unit or gear here."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenIDie; }
    static std::vector<GameObjectId> here(const CardContext& ctx) {
        const auto loc = locationHere(ctx.state, ctx.source, ctx.controller);
        const auto* bfl = std::get_if<BattlefieldLocation>(&loc);
        std::vector<GameObjectId> out;
        if (!bfl) return out;
        for (const auto& [id, o] : ctx.state.objects)
            if (id != ctx.source && (o.isUnit() || (o.isGear() && !o.attached_to.has_value())) &&
                o.battlefieldId() == bfl->id)
                out.push_back(id);
        return out;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const int go = confirmOptional(ctx, "Mournful Spirit: kill a unit or gear here", [&] { return !here(ctx).empty(); });
        if (go == -1) return;
        if (go == 0) { if (finalizingChoices(ctx)) return; return; }
        const auto t = pickTarget(ctx, "Mournful Spirit: kill", here(ctx));
        if (ctx.executor.hasPendingChoice()) return;
        if (finalizingChoices(ctx)) return;
        if (t != kInvalidId && ctx.state.objectExists(t) && ctx.state.getObject(t).location) ctx.executor.killObject(t);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1051;
        d.def_id = R"RB(rad-126-167)RB";
        d.name = R"RB(Mournful Spirit)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-126/167)RB";
        d.collector_number = 126;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 4;
        d.might = 1;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Deathknell);
        d.ability_text = R"RB([Deathknell][>] You may kill a unit or gear here. (When I die, get the effect.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/6d8500ac7efc5382985f566212d88ac8f41c135d-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1051(CardRegistry& r) {
    r.registerCard(1051, std::make_unique<MournfulSpirit>());
}

}  // namespace riftbound
