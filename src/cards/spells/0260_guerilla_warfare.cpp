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

class GuerillaWarfare : public SpellCard {
public:
    const CardDef& def() const override { return def_; }

    // The player chooses up to two [Hidden] cards in their trash, one at a
    // time (declining stops). Then hiding ignores its [A] cost this turn
    // (PlayerState::hide_ignoring_costs_this_turn).
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ps = ctx.state.player(ctx.controller);
        const CardDB& db = ctx.executor.cardDB();
        for (int returned = 0; returned < 2; ++returned) {
            std::vector<GameObjectId> hidden;
            for (auto cid : ps.trash)
                if (ctx.state.objectExists(cid) && ctx.state.getObject(cid).card_def_id != kInvalidId &&
                    db.get(ctx.state.getObject(cid).card_def_id).keywords.has(Keyword::Hidden))
                    hidden.push_back(cid);
            if (hidden.empty()) break;
            const GameObjectId pick = ctx.executor.chooseObjectOrNoneNow(ctx.controller, hidden);
            if (pick == kInvalidId) break;
            ps.trash.erase(std::find(ps.trash.begin(), ps.trash.end(), pick));
            auto& obj = ctx.state.getObject(pick);
            obj.zone = ZoneType::Hand;
            obj.location = std::nullopt;
            ps.hand.push_back(pick);
            ctx.events.logTrace("GUERILLA WARFARE: returned " + obj.name + " from trash to hand");
        }
        ps.hide_ignoring_costs_this_turn = true;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 260;
        d.def_id = R"RB(ogn-264-298)RB";
        d.name = R"RB(Guerilla Warfare)RB";
        d.set_code = R"RB(OGN)RB";
        d.set_name = R"RB(Origins)RB";
        d.public_code = R"RB(OGN-264/298)RB";
        d.collector_number = 264;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.super_type = SuperType::Signature;
        d.domains = {Domain::Mind, Domain::Chaos};
        d.tags = {R"RB(Teemo)RB"};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Return up to two cards with [Hidden] from your trash to your hand. You can hide cards ignoring costs this turn.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/3a1f70e49f1fdbac907a3395f517e96717e8afea-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_260(CardRegistry& r) {
    r.registerCard(260, std::make_unique<GuerillaWarfare>());
}

} // namespace riftbound
