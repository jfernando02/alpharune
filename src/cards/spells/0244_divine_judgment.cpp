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

class DivineJudgment : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Each player chooses 2 units, 2 gear, 2 runes, and 2 cards in their hands.
    //  Recycle the rest."
    // Each player keeps 2 of each category, choosing them with their own
    // agent (two picks per category that has more than 2); the rest are
    // recycled.
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        for (auto p : {ctx.controller, opponent(ctx.controller)}) {
            recycleExcept(ctx, p);
        }
        ctx.events.logTrace("DIVINE JUDGMENT: each player keeps 2/category, recycle rest");
    }

private:
    // The ones `p` doesn't keep out of `cands` (keeps 2, chosen by `p`).
    static std::vector<GameObjectId> notKept(CardContext& ctx, PlayerId p,
                                             std::vector<GameObjectId> cands) {
        if (cands.size() <= 2) return {};
        for (int k = 0; k < 2; ++k) {
            const auto keep = ctx.executor.chooseObjectNow(p, cands);
            auto it = std::find(cands.begin(), cands.end(), keep);
            if (it == cands.end()) it = cands.begin();
            cands.erase(it);
        }
        return cands;
    }
    void recycleExcept(CardContext& ctx, PlayerId p) const {
        std::vector<GameObjectId> units, gear, runes;   // on the board
        for (auto& [id, obj] : ctx.state.objects) {
            if (obj.controller != p || !obj.location.has_value()) continue;
            if (obj.isUnit()) units.push_back(id);
            else if (obj.isGear()) gear.push_back(id);
            else if (obj.isRune()) runes.push_back(id);
        }
        auto& ps = ctx.state.player(p);
        std::vector<GameObjectId> to_recycle;
        for (auto* cat : {&units, &gear, &runes}) {
            auto out = notKept(ctx, p, *cat);
            to_recycle.insert(to_recycle.end(), out.begin(), out.end());
        }
        // Hand cards: the ones not kept leave the hand first.
        std::vector<GameObjectId> hand(ps.hand.begin(), ps.hand.end());
        const auto hand_recycle = notKept(ctx, p, hand);
        for (auto cid : hand_recycle) {
            auto it = std::find(ps.hand.begin(), ps.hand.end(), cid);
            if (it != ps.hand.end()) ps.hand.erase(it);
        }
        to_recycle.insert(to_recycle.end(), hand_recycle.begin(), hand_recycle.end());

        if (!to_recycle.empty())
            ctx.executor.recycleCards(p, to_recycle);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 244;
        d.def_id = R"RB(ogn-244-298)RB";
        d.name = R"RB(Divine Judgment)RB";
        d.set_code = R"RB(OGN)RB";
        d.set_name = R"RB(Origins)RB";
        d.public_code = R"RB(OGN-244/298)RB";
        d.collector_number = 244;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Order};
        d.energy_cost = 7;
        d.power_cost = 2;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Each player chooses 2 units, 2 gear, 2 runes, and 2 cards in their hands. Recycle the rest.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/995778eea2e24fdc62ade38c2baa25b9f1e6ab79-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_244(CardRegistry& r) {
    r.registerCard(244, std::make_unique<DivineJudgment>());
}

} // namespace riftbound
