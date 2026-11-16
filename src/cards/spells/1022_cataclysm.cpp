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

class Cataclysm : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Opponents can't play spells this turn. Move a friendly unit to an occupied
    // enemy battlefield."
    static std::vector<BattlefieldId> enemyBfs(const GameState& s, PlayerId me) {
        std::vector<BattlefieldId> out;
        for (const auto& bf : s.battlefields) {
            if (!bf.controller.has_value() || *bf.controller == me) continue;
            for (const auto& [id, o] : s.objects)
                if (o.isUnit() && o.controller != me && o.battlefieldId() == bf.id) { out.push_back(bf.id); break; }
        }
        return out;
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        ctx.state.player(opponent(ctx.controller)).cant_play_spells_this_turn = true;
        const auto dests = enemyBfs(ctx.state, ctx.controller);
        if (dests.empty()) return;
        const int bf = pickBattlefield(*this, ctx, "Cataclysm: an occupied enemy battlefield",
            [&](BattlefieldId b) { return std::find(dests.begin(), dests.end(), b) != dests.end(); });
        if (bf < 0) return;
        const auto u = pickTarget(ctx, "Cataclysm: move a friendly unit",
            chooseableUnits(ctx.state, ctx.controller, [&](const GameObject& o) {
                return o.controller == ctx.controller && o.battlefieldId() != static_cast<BattlefieldId>(bf); }));
        if (ctx.executor.hasPendingChoice()) return;
        if (u != kInvalidId && ctx.state.objectExists(u) && ctx.state.getObject(u).location)
            ctx.executor.moveToBattlefield(u, static_cast<BattlefieldId>(bf));
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1022;
        d.def_id = R"RB(rad-084-167)RB";
        d.name = R"RB(Cataclysm)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-084/167)RB";
        d.collector_number = 84;
        d.artist = R"RB(Polar Engine Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Body};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(Opponents can't play spells this turn.
Move a friendly unit to an occupied enemy battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/dc551ece6cf007c76a3a16feba5f26fb7dd4ed65-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1022(CardRegistry& r) {
    r.registerCard(1022, std::make_unique<Cataclysm>());
}

}  // namespace riftbound
