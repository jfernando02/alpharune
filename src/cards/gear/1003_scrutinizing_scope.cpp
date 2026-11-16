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

class ScrutinizingScope : public GearCard {
public:
    const CardDef& def() const override { return def_; }
    // "[1], [E]: Choose an opponent. They guess a card type. Reveal the top card
    // of your Main Deck. If it has that type, recycle it. Otherwise, draw it."
    bool hasActivatedAbility() const override { return true; }
    ActivationCost getActivationCost() const override { ActivationCost c; c.energy = 1; c.exhaust = true; return c; }
    void onActivate(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (!ctx.state.chain.resuming.has_value()) return;
        auto& ri = *ctx.state.chain.resuming;
        auto& ps = ctx.state.player(ctx.controller);
        if (ri.resume_point < 20) {
            if (ps.main_deck.empty()) return;
            const PlayerId opp = opponent(ctx.controller);
            ctx.executor.requestChoice(opp, {valChoice(opp, 0, "Unit"), valChoice(opp, 1, "Gear"),
                                             valChoice(opp, 2, "Spell")},
                                       "Scrutinizing Scope: guess a card type");
            ri.resume_point = 20;
            return;
        }
        const int guess = takenValue(ctx, 0);
        if (ps.main_deck.empty()) return;
        const auto top = ps.main_deck.back();
        const auto& c = ctx.state.getObject(top);
        ctx.events.emit(CardRevealedEvent{top, c.card_def_id, c.owner, true, ctx.controller, ZoneType::MainDeck});
        const bool hit = (guess == 0 && c.isUnit()) || (guess == 1 && c.isGear()) || (guess == 2 && c.isSpell());
        if (hit) {
            ps.main_deck.pop_back();
            ps.main_deck.insert(ps.main_deck.begin(), top);
            ctx.events.logTrace("SCRUTINIZING SCOPE: guessed right, recycled " + c.name);
        } else {
            ctx.executor.drawCards(ctx.controller, 1);
            ctx.events.logTrace("SCRUTINIZING SCOPE: guessed wrong, drew the card");
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1003;
        d.def_id = R"RB(rad-064-167)RB";
        d.name = R"RB(Scrutinizing Scope)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-064/167)RB";
        d.collector_number = 64;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Gear;
        d.domains = {Domain::Mind};
        d.energy_cost = 2;
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB([1], [E]: Choose an opponent. They guess a card type. Reveal the top card of your Main Deck. If it has that type, recycle it. Otherwise, draw it. (Unit, gear, and spell are types.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/6a028fc372c14dda73864aba3d3e84063f6da98d-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1003(CardRegistry& r) {
    r.registerCard(1003, std::make_unique<ScrutinizingScope>());
}

}  // namespace riftbound
