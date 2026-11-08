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

// "As an additional cost to play me, return a friendly unit to its owner's
// hand. [Ganking]"
// As for Legion Quartermaster (no structured pre-play cost hook): the play is
// gated on having a friendly unit on the board, and the return is a
// WhenYouPlayMe effect chosen at finalization. NOTE: it happens just after I
// enter rather than strictly before.
class EkkoIngenious : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    static std::vector<GameObjectId> friendlyUnits(const GameState& state, PlayerId controller,
                                                   GameObjectId except) {
        std::vector<GameObjectId> out;
        for (const auto& [id, obj] : state.objects)
            if (id != except && obj.isUnit() && obj.controller == controller &&
                obj.location.has_value())
                out.push_back(id);
        return out;
    }
    bool canBePlayed(const GameState& state, PlayerId player) const override {
        return !friendlyUnits(state, player, kInvalidId).empty();
    }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const GameObjectId unit = pickTarget(ctx, "Ekko, Ingenious: return a friendly unit to hand",
                                             friendlyUnits(ctx.state, ctx.controller, ctx.source));
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (unit == kInvalidId || !ctx.state.objectExists(unit)) return;
        ctx.executor.bounceToHand(unit);
        ctx.events.logTrace("EKKO, INGENIOUS: returned a friendly unit to hand (additional cost)");
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1007;
        d.def_id = R"RB(rad-061-167)RB";
        d.name = R"RB(Ekko, Ingenious)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-061/167)RB";
        d.collector_number = 61;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.domains = {Domain::Mind};
        d.tags = {R"RB(Ekko)RB", R"RB(Zaun)RB"};
        d.energy_cost = 3;
        d.might = 4;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Ganking);
        d.ability_text = R"RB(As an additional cost to play me, return a friendly unit to its owner's hand.
[Ganking] (I can move from battlefield to battlefield.))RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/3129469742d7aa5c6921ebae841ccee1923bb279-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1007(CardRegistry& r) {
    r.registerCard(1007, std::make_unique<EkkoIngenious>());
}

} // namespace riftbound
