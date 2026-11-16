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

class BattlefieldArmorer : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When a friendly gear at a battlefield dies, you may pay [1] to return it
    // to its owner's hand."
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenAFriendlyGearDies; }
    bool triggerCondition(const GameState& s, GameObjectId, PlayerId, TriggerType, GameObjectId subject) const override {
        if (subject == kInvalidId || !s.objectExists(subject)) return false;
        const auto& g = s.getObject(subject);
        const auto it = g.card_counters.find("__died_at_bf");
        return !g.is_token && it != g.card_counters.end() && it->second == 1;
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto g = triggerSubject(ctx);
        const auto inTrash = [&] {
            if (g == kInvalidId || !ctx.state.objectExists(g)) return false;
            const auto& tr = ctx.state.player(ctx.state.getObject(g).owner).trash;
            return std::find(tr.begin(), tr.end(), g) != tr.end();
        };
        const int go = confirmOptional(ctx, "Battlefield Armorer: pay [1] to return the gear", [&] {
            return costAlreadyPaid(ctx) || payableEnergy(ctx.state, ctx.controller) >= 1; });
        if (go == -1) return;
        bool paid = false;
        if (go == 1 && costDueNow(ctx)) paid = payEnergy(ctx, ctx.controller, 1);
        if (finalizingChoices(ctx)) return;
        if (go != 1 || !(paid || costAlreadyPaid(ctx)) || !inTrash()) return;
        auto& o = ctx.state.getObject(g);
        pullFromZoneLists(ctx.state, g);
        o.zone = ZoneType::Hand;
        o.location = std::nullopt;
        ctx.state.player(o.owner).hand.push_back(g);
        ctx.events.logTrace("BATTLEFIELD ARMORER: returned " + o.name + " to hand");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1054;
        d.def_id = R"RB(rad-130-167)RB";
        d.name = R"RB(Battlefield Armorer)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-130/167)RB";
        d.collector_number = 130;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Order};
        d.energy_cost = 3;
        d.might = 3;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(When a friendly gear at a battlefield dies, you may pay [1] to return it to its owner's hand.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/eb04c39763de8fb8e56153d1f0e4f799e15dced9-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1054(CardRegistry& r) {
    r.registerCard(1054, std::make_unique<BattlefieldArmorer>());
}

}  // namespace riftbound
