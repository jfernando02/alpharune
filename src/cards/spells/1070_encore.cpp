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

class Encore : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Choose two — Deal 2 to a unit. / Play two 1 [M] Recruit unit tokens. /
    // Give up to two units each +1 [M] this turn. / Look at the top 2 cards of
    // your Main Deck, draw 1 and recycle the other."
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        static const int A[6] = {0, 0, 0, 1, 1, 2}, B[6] = {1, 2, 3, 2, 3, 3};
        auto& ri = ctx.state.chain.resuming.value();
        const int m = pickMode(ctx, "Encore: choose two", 6,
            {"Deal 2 + Recruits", "Deal 2 + Might", "Deal 2 + Look", "Recruits + Might", "Recruits + Look", "Might + Look"});
        if (m < 0) return;
        const bool dmg = A[m] == 0, rec = A[m] == 1 || B[m] == 1, mt = A[m] == 2 || B[m] == 2, look = B[m] == 3;
        GameObjectId t = kInvalidId;
        if (dmg) {
            t = pickTarget(ctx, "Encore: deal 2 to a unit", anyUnits(ctx.state, ctx.controller));
            if (ctx.executor.hasPendingChoice()) return;
        }
        std::pair<GameObjectId, GameObjectId> pr{kInvalidId, kInvalidId};
        if (mt) {
            pr = pickTargetPair(ctx, "Encore: +1 [M] to up to two units", anyUnits(ctx.state, ctx.controller),
                                [&](GameObjectId a) { return dropSource(a, anyUnits(ctx.state, ctx.controller)); });
            if (ctx.executor.hasPendingChoice()) return;
        }
        auto& ps = ctx.state.player(ctx.controller);
        if (ri.resume_point < 20) {
            ri.resume_point = 20;
            if (dmg && t != kInvalidId && ctx.state.objectExists(t) && ctx.state.getObject(t).location) dealAndKill(ctx, t, 2);
            if (rec) { createRecruitAtBase(ctx); createRecruitAtBase(ctx); }
            for (auto u : {pr.first, pr.second})
                if (mt && u != kInvalidId && ctx.state.objectExists(u) && ctx.state.getObject(u).location)
                    ctx.executor.giveTemporaryMight(u, 1);
            if (!look || ps.main_deck.empty()) return;
            if (ps.main_deck.size() == 1) { ctx.executor.drawCards(ctx.controller, 1); return; }
            std::vector<Intent> opts;
            for (size_t k = 0; k < 2; ++k) {
                const auto c = ps.main_deck[ps.main_deck.size() - 1 - k];
                Intent i = valChoice(ctx.controller, static_cast<int>(k), ctx.state.getObject(c).name);
                i.chosen_objects = {c};
                opts.push_back(std::move(i));
            }
            ctx.executor.requestChoice(ctx.controller, std::move(opts), "Encore: draw which");
            ri.resume_point = 21;
            return;
        }
        if (ri.resume_point != 21) return;
        ri.resume_point = 22;
        auto ch = ctx.executor.takeChoice();
        if (ps.main_deck.size() < 2) return;
        const auto top = ps.main_deck.back(), second = ps.main_deck[ps.main_deck.size() - 2];
        const bool keepTop = !ch || ch->chosen_objects.empty() || ch->chosen_objects[0] == top;
        const auto other = keepTop ? second : top;
        ps.main_deck.erase(std::find(ps.main_deck.begin(), ps.main_deck.end(), other));
        ps.main_deck.insert(ps.main_deck.begin(), other);
        ctx.executor.drawCards(ctx.controller, 1);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1070;
        d.def_id = R"RB(rad-152-167)RB";
        d.name = R"RB(Encore)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-152/167)RB";
        d.collector_number = 152;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Mind, Domain::Order};
        d.energy_cost = 3;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(Choose two —Deal 2 to a unit.Play two 1 [M] Recruit unit tokens.Give up to two units each +1 [M] this turn.Look at the top 2 cards of your Main Deck, draw 1 and recycle the other.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/0200aeca5d0a1cca4b8ab5b3f3910207118ee4e4-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1070(CardRegistry& r) {
    r.registerCard(1070, std::make_unique<Encore>());
}

}  // namespace riftbound
