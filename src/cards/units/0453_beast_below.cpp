#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

class BeastBelow : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "When you play me, return another friendly unit and an enemy unit to their
    // owners' hands." Both are chosen as the ability is played (CR 383.3); the
    // engine used to give it no targets, and before that only one was returned.
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto units = [&](bool friendly) {
            std::vector<GameObjectId> out;
            for (const auto& [id, obj] : ctx.state.objects) {
                if (!obj.isUnit() || !obj.location.has_value() || id == ctx.source) continue;
                if ((obj.controller == ctx.controller) != friendly) continue;
                if (!friendly && obj.untargetable_by_enemy) continue;
                out.push_back(id);
            }
            return out;
        };
        GameObjectId mine = kInvalidId, theirs = kInvalidId;
        const auto friends = units(true);
        if (!friends.empty()) {
            auto picked = pickTargetPair(ctx, "Beast Below: return which friendly unit, then which enemy unit",
                                         friends, [&](GameObjectId) { return units(false); });
            if (ctx.executor.hasPendingChoice()) return;
            mine = picked.first;
            theirs = picked.second;
        } else {
            theirs = pickTarget(ctx, "Beast Below: return which enemy unit", units(false));
            if (ctx.executor.hasPendingChoice()) return;
        }
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        for (GameObjectId u : {mine, theirs})
            if (u != kInvalidId && ctx.state.objectExists(u) &&
                ctx.state.getObject(u).location.has_value())
                ctx.executor.bounceToHand(u);
    }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 453;
        d.def_id = R"RB(sfd-132-221)RB";
        d.name = R"RB(Beast Below)RB";
        d.set_code = R"RB(SFD)RB";
        d.set_name = R"RB(Spiritforged)RB";
        d.public_code = R"RB(SFD-132/221)RB";
        d.collector_number = 132;
        d.artist = R"RB(Six More Vodka)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};
        d.tags = {R"RB(Bilgewater)RB"};
        d.energy_cost = 7;
        d.power_cost = 2;
        d.might = 8;
        d.rarity = Rarity::Uncommon;
        d.ability_text = R"RB(When you play me, return another friendly unit and an enemy unit to their owners' hands.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/d27a56111c68c651099d7b73b22a54a825458857-744x1039.png?accountingTag=RB)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_453(CardRegistry& r) {
    r.registerCard(453, std::make_unique<BeastBelow>());
}

} // namespace riftbound
