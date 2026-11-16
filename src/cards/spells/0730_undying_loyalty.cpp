#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include "core/card_db.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "cards/card_helpers.h"

namespace riftbound {
namespace {

// "This costs [2] less if you choose a Bird, Cat, Dog, or Poro.
//  Play a unit with cost no more than [2] and no more than [A] from your
//  trash, ignoring its cost."
//
// The unit is chosen as the spell is played (kFinalizeResolve). The [2] less
// is taken when a Bird/Cat/Dog/Poro could be chosen (selfCostReduction, as the
// cost is paid); choosing another unit then charges that [2] back, so such a
// unit is offered only when the [2] can be paid.

class UndyingLoyalty : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    bool needsPlayTimeTarget() const override { return true; }
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }

    // "cost no more than [2] and no more than [A]"
    static bool eligible(const CardDB& db, const GameObject& obj) {
        if (!obj.isUnit() || obj.card_def_id == kInvalidId) return false;
        const auto& d = db.get(obj.card_def_id);
        return d.energy_cost <= 2 && d.power_cost <= 1;
    }
    static bool tagged(const GameObject& obj) {
        return hasTag(obj, "Bird") || hasTag(obj, "Cat") || hasTag(obj, "Dog") || hasTag(obj, "Poro");
    }
    static bool discounted(const GameState& state, PlayerId player) {
        for (auto cid : state.player(player).trash) {
            if (!state.objectExists(cid)) continue;
            const auto& obj = state.getObject(cid);
            if (obj.isUnit() && tagged(obj) && obj.card_def_id != kInvalidId) return true;
        }
        return false;
    }

    int selfCostReduction(const GameState& state, PlayerId player) const override {
        // Energy/Power gates are checked on the pick (no CardDB here).
        return discounted(state, player) ? 2 : 0;
    }

    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        const auto& db = ctx.executor.cardDB();
        const bool disc = discounted(ctx.state, ctx.controller);
        const bool can_pay_back = costAlreadyPaid(ctx) || payableEnergy(ctx.state, ctx.controller) >= 2;
        std::vector<GameObjectId> legal;
        for (auto cid : ps.trash) {
            if (!ctx.state.objectExists(cid)) continue;
            const auto& obj = ctx.state.getObject(cid);
            if (!eligible(db, obj)) continue;
            if (disc && !tagged(obj) && !can_pay_back) continue;
            legal.push_back(cid);
        }
        if (legal.empty()) { if (finalizingChoices(ctx)) return; return; }
        GameObjectId picked = pickTarget(ctx, "Undying Loyalty (unit from trash)", legal);
        if (ctx.executor.hasPendingChoice()) return;   // suspended
        if (disc && picked != kInvalidId && ctx.state.objectExists(picked) &&
            !tagged(ctx.state.getObject(picked)) && costDueNow(ctx))
            payEnergy(ctx, ctx.controller, 2);   // the [2] less no longer applies
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (picked == kInvalidId || !ctx.state.objectExists(picked)) return;
        auto it = std::find(ps.trash.begin(), ps.trash.end(), picked);
        if (it == ps.trash.end()) return;
        ps.trash.erase(it);
        ctx.executor.playIgnoringCost(ctx.controller, picked);
        ctx.events.logTrace("UNDYING LOYALTY: played a unit from trash, ignoring cost");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 730;
        d.def_id = R"RB(unl-168-219)RB";
        d.name = R"RB(Undying Loyalty)RB";
        d.set_code = R"RB(UNL)RB";
        d.set_name = R"RB(Unleashed)RB";
        d.public_code = R"RB(UNL-168/219)RB";
        d.collector_number = 168;
        d.artist = R"RB(Max Grecke)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Order};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(This costs [2] less if you choose a Bird, Cat, Dog, or Poro.
Play a unit with cost no more than [2] and no more than [A] from your trash, ignoring its cost.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/703b85f8284ed13865012289d03602e2cd24f4b2-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_730(CardRegistry& r) {
    r.registerCard(730, std::make_unique<UndyingLoyalty>());
}

} // namespace riftbound
