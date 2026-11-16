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
    // APPROX: X is chosen and paid at resolution; units return to the same location.
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        if (ri.resume_data.size() < 12) ri.resume_data.resize(12, 0);
        if (ri.resume_point < 20) {
            int power = 0;
            const auto& pool = ctx.state.player(ctx.controller).rune_pool;
            power = pool.totalPower() + pool.universal_power;
            for (const auto& [id, o] : ctx.state.objects)
                if (o.isRune() && o.controller == ctx.controller && o.isAtBase()) ++power;
            const int maxX = std::min<int>(power, static_cast<int>(friendlyUnits(ctx.state, ctx.controller).size()));
            const int x = pickXAmount(ctx, "Chronobreak: pay X Power", 0, maxX);
            if (x < 0) return;
            int paid = 0;
            for (int i = 0; i < x; ++i)
                if (payOnePower(ctx, ctx.controller, std::nullopt)) ++paid;
            ri.resume_data[10] = paid;
            ri.resume_data[11] = 0;
            ri.resume_point = 20;
        }
        while (ri.resume_data[11] < ri.resume_data[10]) {
            if (ri.resume_point == 20) {
                std::vector<Intent> opts;
                for (auto u : friendlyUnits(ctx.state, ctx.controller)) {
                    if (ctx.state.getObject(u).card_counters.count("__chrono")) continue;
                    Intent i = valChoice(ctx.controller, 0, ctx.state.getObject(u).name);
                    i.chosen_objects = {u};
                    opts.push_back(std::move(i));
                }
                if (opts.empty()) break;
                ctx.executor.requestChoice(ctx.controller, std::move(opts), "Chronobreak: banish and replay a unit");
                ri.resume_point = 21;
                return;
            }
            ri.resume_point = 20;
            ++ri.resume_data[11];
            auto ch = ctx.executor.takeChoice();
            if (!ch || ch->chosen_objects.empty()) continue;
            const auto u = ch->chosen_objects[0];
            if (!ctx.state.objectExists(u) || !ctx.state.getObject(u).location) continue;
            const auto loc = *ctx.state.getObject(u).location;
            const auto owner = ctx.state.getObject(u).owner;
            const bool token = ctx.state.getObject(u).is_token;
            ctx.executor.banishObject(u);
            if (token || !ctx.state.objectExists(u)) continue;
            pullFromZoneLists(ctx.state, u);
            ctx.executor.playIgnoringCost(owner, u, loc);
            if (ctx.state.objectExists(u)) ctx.state.getObject(u).card_counters["__chrono"] = 1;
        }
        for (auto u : friendlyUnits(ctx.state, ctx.controller)) ctx.state.getObject(u).card_counters.erase("__chrono");
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
