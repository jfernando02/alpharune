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

class ScorchedEarth : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Deal 5 damage split among units at a single battlefield."
    // The battlefield, then each point of damage, is chosen as it resolves.
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        const int bf = pickBattlefield(*this, ctx, "Scorched Earth: a battlefield",
            [&](BattlefieldId b) { return !unitsAtBattlefield(ctx.state, ctx.controller, b).empty(); });
        if (bf == -1) return;
        if (bf < 0) return;
        if (ri.resume_point < 20) ri.resume_point = 20;
        while (ri.resume_data.size() < 11) ri.resume_data.push_back(0);
        if (ri.resume_point == 21) {   // a point was assigned
            const auto id = takenObject(ctx);
            ri.resume_data.push_back(id);
            ri.resume_data[10]++;
            ri.resume_point = 20;
        }
        const auto here = unitsAtBattlefield(ctx.state, ctx.controller, static_cast<BattlefieldId>(bf));
        if (ri.resume_data[10] < 5 && !here.empty()) {
            std::vector<Intent> opts;
            for (auto u : here) opts.push_back(objChoice(ctx.controller, u));
            ctx.executor.requestChoice(ctx.controller, std::move(opts),
                "Scorched Earth: 1 damage (" + std::to_string(ri.resume_data[10] + 1) + "/5)");
            ri.resume_point = 21;
            return;
        }
        std::vector<std::pair<GameObjectId, int>> split;
        for (size_t i = 11; i < ri.resume_data.size(); ++i) {
            const GameObjectId u = ri.resume_data[i];
            auto it = std::find_if(split.begin(), split.end(), [&](auto& p) { return p.first == u; });
            if (it == split.end()) split.push_back({u, 1}); else it->second++;
        }
        std::vector<GameObjectId> hit;
        for (auto& [u, n] : split)
            if (ctx.state.objectExists(u) && ctx.state.getObject(u).battlefieldId() == static_cast<BattlefieldId>(bf)) {
                ctx.executor.dealDamage(u, n, ctx.source);
                hit.push_back(u);
            }
        killLethal(ctx, hit);
        ctx.events.logTrace("SCORCHED EARTH: 5 damage split among " + std::to_string(hit.size()) + " unit(s)");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 961;
        d.def_id = R"RB(rad-006-167)RB";
        d.name = R"RB(Scorched Earth)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-006/167)RB";
        d.collector_number = 6;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury};
        d.energy_cost = 4;
        d.power_cost = 1;
        d.rarity = Rarity::Common;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Deal 5 damage split among units at a single battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/e646f9850571e20eca22e07af5d4dd68813fdd19-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_961(CardRegistry& r) {
    r.registerCard(961, std::make_unique<ScorchedEarth>());
}

}  // namespace riftbound
