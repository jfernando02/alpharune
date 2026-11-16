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

class SirensSong : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Move an enemy unit that moved this turn."
    static std::vector<GameObjectId> movers(const GameState& s, PlayerId p) {
        return chooseableUnits(s, p, [&](const GameObject& o) { return o.controller != p && o.moves_this_turn > 0; });
    }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !movers(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        const auto u = pickTarget(ctx, "Siren's Song: an enemy unit that moved", movers(ctx.state, ctx.controller));
        if (ctx.executor.hasPendingChoice()) return;
        if (u == kInvalidId || !ctx.state.objectExists(u) || !ctx.state.getObject(u).location) return;
        if (ri.resume_point < 20) {
            std::vector<Intent> opts;
            for (int d : moveDests(ctx.state, ctx.state.getObject(u)))
                opts.push_back(valChoice(ctx.controller, d, destLabel(d)));
            if (opts.empty()) return;
            ctx.executor.requestChoice(ctx.controller, std::move(opts), "Siren's Song: move it where");
            ri.resume_point = 20;
            return;
        }
        if (ri.resume_point == 20) { ri.resume_point = 21; moveTo(ctx, u, takenValue(ctx, 0)); }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1032;
        d.def_id = R"RB(rad-098-167)RB";
        d.name = R"RB(Siren's Song)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-098/167)RB";
        d.collector_number = 98;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Chaos};
        d.energy_cost = 2;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Move an enemy unit that moved this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/742967b3e85f51b242fc04b8db4c4e3cd7b88672-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1032(CardRegistry& r) {
    r.registerCard(1032, std::make_unique<SirensSong>());
}

}  // namespace riftbound
