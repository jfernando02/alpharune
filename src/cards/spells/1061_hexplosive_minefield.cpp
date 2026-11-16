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

class HexplosiveMinefield : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Kill any number of friendly Bomb tokens. At end of turn, play that many
    // Bomb tokens to a single battlefield."
    // APPROX: the end-of-turn battlefield is the one with the most enemy units.
    static std::vector<GameObjectId> bombs(const GameState& s, PlayerId p) {
        std::vector<GameObjectId> out;
        for (const auto& [id, o] : s.objects)
            if (o.controller == p && o.location.has_value() && o.card_def_id == kBombTokenCardDefId) out.push_back(id);
        return out;
    }
    bool hasLegalTargets(const GameState& s, PlayerId p) const override { return !bombs(s, p).empty(); }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        auto& ri = ctx.state.chain.resuming.value();
        if (ri.resume_data.size() < 11) ri.resume_data.resize(11, 0);
        if (ri.resume_point == 21) {
            auto ch = ctx.executor.takeChoice();
            if (ch && !ch->chosen_objects.empty() && ctx.state.objectExists(ch->chosen_objects[0])) {
                ctx.executor.killObject(ch->chosen_objects[0]);
                ++ri.resume_data[10];
            } else {
                ri.resume_point = 30;
            }
        }
        if (ri.resume_point < 30) {
            const auto bs = bombs(ctx.state, ctx.controller);
            if (!bs.empty()) {
                std::vector<Intent> opts{valChoice(ctx.controller, -1, "Stop")};
                for (auto b : bs) { Intent i = valChoice(ctx.controller, 0, "Bomb"); i.chosen_objects = {b}; opts.push_back(std::move(i)); }
                ctx.executor.requestChoice(ctx.controller, std::move(opts), "Hexplosive Minefield: kill a Bomb");
                ri.resume_point = 21;
                return;
            }
        }
        ri.resume_point = 30;
        if (ri.resume_data[10] <= 0 || !ctx.state.objectExists(ctx.source)) return;
        ctx.state.getObject(ctx.source).card_counters["__bombs"] = ri.resume_data[10];
        DelayedAbility da;
        da.source = ctx.source;
        da.card_def_id = cardDefId();
        da.controller = ctx.controller;
        da.trigger = TriggerType::AtEndOfTurn;
        da.expires_on_turn = ctx.state.turn.turn_number;
        ctx.state.delayed_abilities.push_back(da);
    }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.objectExists(ctx.source)) return;
        auto& me = ctx.state.getObject(ctx.source);
        const int n = me.card_counters["__bombs"];
        me.card_counters.erase("__bombs");
        if (n <= 0 || ctx.state.battlefields.empty()) return;
        BattlefieldId best = ctx.state.battlefields.front().id;
        size_t most = 0;
        for (const auto& bf : ctx.state.battlefields) {
            const auto e = enemyUnitsAt(ctx.state, ctx.controller, bf.id).size();
            if (e > most) { most = e; best = bf.id; }
        }
        for (int i = 0; i < n; ++i) createBomb(ctx, best);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1061;
        d.def_id = R"RB(rad-142-167)RB";
        d.name = R"RB(Hexplosive Minefield)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-142/167)RB";
        d.collector_number = 142;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Fury, Domain::Chaos};
        d.energy_cost = 2;
        d.power_cost = 1;
        d.rarity = Rarity::Epic;
        d.keywords.set(Keyword::Action);
        d.ability_text = R"RB([Action] (Play on your turn or in showdowns.)
Kill any number of friendly Bomb tokens. At end of turn, play that many Bomb tokens to a single battlefield.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/65b599befcc6ed873de0e7965bf497a805505327-744x1040.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1061(CardRegistry& r) {
    r.registerCard(1061, std::make_unique<HexplosiveMinefield>());
}

}  // namespace riftbound
