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

class IronRevenant : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Reaction][>] Banish a unit or gear from your trash, [E]: [Add] [A].
    // Spend this Power only to play units or gear."
    // The player picks the unit or gear to banish (a cost of the ability).
    static GameObjectId fodder(const GameState& s, PlayerId p) {
        for (auto c : s.player(p).trash) {
            if (!s.objectExists(c)) continue;
            const auto& o = s.getObject(c);
            if (!o.isUnit() && !o.isGear()) continue;
            return c;
        }
        return kInvalidId;
    }
    bool hasActivatedAbility() const override { return true; }
    bool isReactionAbility() const override { return true; }
    bool canActivateAbility(const GameState& s, PlayerId p) const override { return fodder(s, p) != kInvalidId; }
    ActivationCost getActivationCost() const override { ActivationCost c; c.exhaust = true; return c; }
    void onActivate(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        std::vector<GameObjectId> options;
        for (auto c : ctx.state.player(ctx.controller).trash)
            if (ctx.state.objectExists(c) &&
                (ctx.state.getObject(c).isUnit() || ctx.state.getObject(c).isGear()))
                options.push_back(c);
        const auto f = ctx.executor.chooseObjectNow(ctx.controller, options);
        if (f == kInvalidId) return;

        ctx.executor.banishObject(f);
        ctx.executor.addRestrictedPower(ctx.controller, SpendScope::UnitsOrGear, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1062;
        d.def_id = R"RB(rad-143-167)RB";
        d.name = R"RB(Iron Revenant)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-143/167)RB";
        d.collector_number = 143;
        d.artist = R"RB(TUTU)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Fury, Domain::Order};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB([Reaction][>] Banish a unit or gear from your trash, [E]: [Add] [A]. Spend this Power only to play units or gear. (Abilities that add resources can't be reacted to.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/90ec3319518771820909592eee963d34dab037bc-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1062(CardRegistry& r) {
    r.registerCard(1062, std::make_unique<IronRevenant>());
}

}  // namespace riftbound
