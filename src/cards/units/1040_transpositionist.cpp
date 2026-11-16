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

class Transpositionist : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "[P], [E]: Return a friendly unit to its owner's hand. You may play a unit
    // with less Might than that unit had, ignoring its cost."
    bool hasActivatedAbility() const override { return true; }
    ActivationCost getActivationCost() const override {
        ActivationCost c; c.exhaust = true; c.power = 1; c.power_domain = Domain::Chaos; return c;
    }
    void onActivate(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.chain.resuming.has_value()) return;
        auto& ri = *ctx.state.chain.resuming;
        if (ri.resume_data.size() < 11) ri.resume_data.resize(11, 0);
        auto& ps = ctx.state.player(ctx.controller);
        if (ri.resume_point < 20) {
            const auto t = pickTarget(ctx, "Transpositionist: return a friendly unit", friendlyUnits(ctx.state, ctx.controller));
            if (ctx.executor.hasPendingChoice()) return;
            if (t == kInvalidId || !ctx.state.objectExists(t)) return;
            ri.resume_data[10] = ctx.state.getObject(t).current_might;
            ctx.executor.bounceToHand(t);
            std::vector<Intent> opts{valChoice(ctx.controller, -1, "Play nothing")};
            for (auto c : ps.hand) {
                const auto& o = ctx.state.getObject(c);
                if (!o.isUnit() || o.base_might >= ri.resume_data[10]) continue;
                Intent i = valChoice(ctx.controller, 0, o.name);
                i.chosen_objects = {c};
                opts.push_back(std::move(i));
            }
            if (opts.size() == 1) return;
            ctx.executor.requestChoice(ctx.controller, std::move(opts), "Transpositionist: play a smaller unit free");
            ri.resume_point = 20;
            return;
        }
        if (ri.resume_point != 20) return;
        ri.resume_point = 21;
        auto ch = ctx.executor.takeChoice();
        if (!ch || ch->chosen_objects.empty()) return;
        const auto c = ch->chosen_objects[0];
        if (std::find(ps.hand.begin(), ps.hand.end(), c) == ps.hand.end()) return;
        pullFromZoneLists(ctx.state, c);
        ctx.executor.playIgnoringCost(ctx.controller, c);
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1040;
        d.def_id = R"RB(rad-115-167)RB";
        d.name = R"RB(Transpositionist)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-115/167)RB";
        d.collector_number = 115;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Unit;
        d.domains = {Domain::Chaos};
        d.energy_cost = 6;
        d.might = 4;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB([P], [E]: Return a friendly unit to its owner's hand. You may play a unit with less Might than that unit had, ignoring its cost.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/60cf9dbb9345d44f0a9a78b60d5fb71411b50d69-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1040(CardRegistry& r) {
    r.registerCard(1040, std::make_unique<Transpositionist>());
}

}  // namespace riftbound
