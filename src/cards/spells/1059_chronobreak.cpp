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

class Chronobreak : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "As an additional cost to play this, pay any amount of Power. Banish that
    // many units you control, then for each, its owner plays that unit back to
    // the same location or to their base, ignoring its cost."
    // X is chosen and paid as the spell is played (CR 355 — an additional
    // cost), so the finalization pass runs this callback first. Then the
    // units are chosen one by one (the order they come back in), all banished,
    // and each owner picks the same location or their base.
    // resume_data: [9] replay index, [10] X paid, [11] units chosen,
    // [12..27] chosen ids, [30..45] where each was (0 base, 1+i battlefield i, -1 gone).
    unsigned choicesAtFinalization() const override { return kFinalizeResolve; }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        constexpr int kMax = 16, kIdx = 9, kPaid = 10, kCount = 11, kSel = 12, kWhere = 30;
        auto& ri = ctx.state.chain.resuming.value();
        if (ri.resume_data.size() < 48) ri.resume_data.resize(48, 0);
        if (ri.resume_point < 20) {
            int maxX = kMax;
            if (!costAlreadyPaid(ctx)) {
                const auto& pool = ctx.state.player(ctx.controller).rune_pool;
                int power = pool.totalPower() + pool.universal_power;
                for (const auto& [id, o] : ctx.state.objects)
                    if (o.isRune() && o.controller == ctx.controller && o.isAtBase()) ++power;
                maxX = std::min({power, kMax,
                                 static_cast<int>(friendlyUnits(ctx.state, ctx.controller).size())});
            }
            const int x = pickXAmount(ctx, "Chronobreak: pay X Power", 0, maxX);
            if (x < 0) return;
            if (costDueNow(ctx)) {
                int paid = 0;
                const auto purpose = ctx.state.purposePlaying(CardType::Spell);
                for (int i = 0; i < x; ++i)
                    if (payOnePower(ctx, ctx.controller, std::nullopt, purpose)) ++paid;
                ri.resume_data[kPaid] = paid;
            }
            if (finalizingChoices(ctx)) return;   // choices and costs end here (CR 383.3)
            ri.resume_data[kCount] = 0;
            ri.resume_data[kIdx] = 0;
            ri.resume_point = 20;
        }
        auto chosen = [&](GameObjectId u) {
            for (int k = 0; k < ri.resume_data[kCount]; ++k)
                if (ri.resume_data[kSel + k] == static_cast<int>(u)) return true;
            return false;
        };
        // 20/21: choose the units.
        while (ri.resume_point == 20 || ri.resume_point == 21) {
            if (ri.resume_point == 21) {
                ri.resume_point = 20;
                auto ch = ctx.executor.takeChoice();
                if (!ch || ch->chosen_objects.empty()) { ri.resume_point = 22; break; }
                const auto u = ch->chosen_objects[0];
                if (ctx.state.objectExists(u) && !chosen(u))
                    ri.resume_data[kSel + ri.resume_data[kCount]++] = static_cast<int>(u);
            }
            if (ri.resume_data[kCount] >= ri.resume_data[kPaid]) { ri.resume_point = 22; break; }
            std::vector<Intent> opts;
            for (auto u : friendlyUnits(ctx.state, ctx.controller)) {
                if (chosen(u)) continue;
                Intent i = valChoice(ctx.controller, 0, ctx.state.getObject(u).name);
                i.chosen_objects = {u};
                opts.push_back(std::move(i));
            }
            if (opts.empty()) { ri.resume_point = 22; break; }
            ctx.executor.requestChoice(ctx.controller, std::move(opts),
                                       "Chronobreak: choose a unit to banish (in replay order)");
            ri.resume_point = 21;
            return;
        }
        // 22: banish them all.
        if (ri.resume_point == 22) {
            for (int k = 0; k < ri.resume_data[kCount]; ++k) {
                const auto u = static_cast<GameObjectId>(ri.resume_data[kSel + k]);
                int where = -1;
                if (ctx.state.objectExists(u) && ctx.state.getObject(u).location.has_value()) {
                    const auto& o = ctx.state.getObject(u);
                    const auto bf = o.battlefieldId();
                    where = bf ? 1 + static_cast<int>(*bf) : 0;
                    const bool token = o.is_token;
                    ctx.executor.banishObject(u);
                    if (token || !ctx.state.objectExists(u)) where = -1;
                }
                ri.resume_data[kWhere + k] = where;
            }
            ri.resume_point = 23;
        }
        // 23/24: each owner plays theirs back, to the same location or their base.
        while (ri.resume_point == 23 || ri.resume_point == 24) {
            int& k = ri.resume_data[kIdx];
            if (k >= ri.resume_data[kCount]) break;
            const auto u = static_cast<GameObjectId>(ri.resume_data[kSel + k]);
            const int where = ri.resume_data[kWhere + k];
            if (where < 0 || !ctx.state.objectExists(u)) { ++k; ri.resume_point = 23; continue; }
            const auto owner = ctx.state.getObject(u).owner;
            bool same = false;
            if (where > 0) {
                if (ri.resume_point == 23) {
                    std::vector<Intent> opts = {valChoice(owner, 1, "Same battlefield"),
                                                valChoice(owner, 0, "Base")};
                    ctx.executor.requestChoice(owner, std::move(opts),
                                               "Chronobreak: play it back where?");
                    ri.resume_point = 24;
                    return;
                }
                same = takenValue(ctx, 1) == 1;
            }
            ri.resume_point = 23;
            ++k;
            LocationId loc = BaseLocation{owner};
            if (same) loc = BattlefieldLocation{static_cast<BattlefieldId>(where - 1)};
            pullFromZoneLists(ctx.state, u);
            ctx.executor.playIgnoringCost(owner, u, loc);
        }
        ri.resume_point = 30;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1059;
        d.def_id = R"RB(rad-140-167)RB";
        d.name = R"RB(Chronobreak)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-140/167)RB";
        d.collector_number = 140;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury, Domain::Mind};
        d.energy_cost = 2;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB(As an additional cost to play this, pay any amount of Power.
Banish that many units you control, then for each, its owner plays that unit back to the same location or to their base, ignoring its cost. (You choose the order.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/af2123a99f3e3e7e6c9b101c5fc2c9b0ff56e096-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1059(CardRegistry& r) {
    r.registerCard(1059, std::make_unique<Chronobreak>());
}

}  // namespace riftbound
