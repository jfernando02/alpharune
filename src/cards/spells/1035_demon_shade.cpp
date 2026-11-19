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

class DemonShade : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "[Hidden] Choose a friendly unit, then choose one — Move it to an open
    // battlefield. / Give it [Ganking] this turn if there are no open battlefields."
    static std::vector<BattlefieldId> openBfs(const GameState& s) {
        std::vector<BattlefieldId> out;
        for (const auto& bf : s.battlefields) if (!bf.controller.has_value()) out.push_back(bf.id);
        return out;
    }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !friendlyUnits(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        const auto open = openBfs(ctx.state);
        const int mode = pickMode(ctx, "Demon Shade", 2, {"move it to an open battlefield", "give it [Ganking] this turn"},
                                  open.empty() ? 2u : 1u);
        if (mode == -1) return;
        if (mode < 0) return;
        const auto u = pickTarget(ctx, "Demon Shade: a friendly unit", friendlyUnits(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (u == kInvalidId || !ctx.state.objectExists(u) || !ctx.state.getObject(u).location) return;
        if (mode == 1) { ctx.executor.giveTemporaryKeyword(u, Keyword::Ganking, 0); return; }
        if (ri.resume_point < 20) {
            std::vector<Intent> opts;
            for (auto b : open)
                if (ctx.state.getObject(u).battlefieldId() != b)
                    opts.push_back(valChoice(ctx.controller, 1 + static_cast<int>(b), destLabel(1 + static_cast<int>(b))));
            if (opts.empty()) return;
            ctx.executor.requestChoice(ctx.controller, std::move(opts), "Demon Shade: which open battlefield");
            ri.resume_point = 20;
            return;
        }
        if (ri.resume_point == 20) { ri.resume_point = 21; moveTo(ctx, u, takenValue(ctx, 0)); }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1035;
        d.def_id = R"RB(rad-102-167)RB";
        d.name = R"RB(Demon Shade)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-102/167)RB";
        d.collector_number = 102;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Chaos};  // the gallery says Fury (a mislabel): its collector number is in the Chaos block, as riftdecks lists it
        d.energy_cost = 3;
        d.rarity = Rarity::Uncommon;
        d.keywords.set(Keyword::Hidden);
        d.ability_text = R"RB([Hidden] (Hide now for [A] to react with later for [0].)
Choose a friendly unit, then choose one —Move it to an open battlefield.Give it [Ganking] this turn if there are no open battlefields. (It can move from battlefield to battlefield.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/e9dd313886624aafc7e0c7f9e1689225c042b484-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1035(CardRegistry& r) {
    r.registerCard(1035, std::make_unique<DemonShade>());
}

}  // namespace riftbound
