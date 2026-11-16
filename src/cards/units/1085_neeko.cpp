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

class Neeko : public UnitCard {
public:
    const CardDef& def() const override { return def_; }
    // "If Neeko is in your deck, choose 3 different legends in addition to your
    // starting legend. When you play me, choose one of those legends and add it to
    // your Legend Zone while I'm on the board."
    // The 3 legends are chosen with the deck (DeckSubmission::extra_legends,
    // PlayerState::reserve_legends); the added legend's legend_anchor is me,
    // and the engine takes it out of the Legend Zone once I leave the board.
    int extraLegendsAtDeckBuilding() const override { return 3; }
    unsigned choicesAtFinalization() const override { return kFinalizeTrigger; }
    TriggerType triggerType() const override { return TriggerType::WhenYouPlayMe; }
    void onTrigger(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        const auto reserve = ctx.state.player(ctx.controller).reserve_legends;
        int m = 0;
        if (reserve.size() > 1) {
            std::vector<std::string> labels;
            for (auto id : reserve) labels.push_back(ctx.executor.cardDB().get(id).name);
            m = pickMode(ctx, "Neeko: add which legend?", static_cast<int>(reserve.size()), labels);
            if (m == -1) return;
        }
        if (finalizingChoices(ctx)) return;   // choices end here (CR 383.3)
        if (reserve.empty() || m < 0 || m >= static_cast<int>(reserve.size())) return;
        if (!ctx.state.objectExists(ctx.source) || !ctx.state.getObject(ctx.source).location) return;
        const auto legend = ctx.executor.instantiateCard(reserve[m], ctx.controller);
        auto& l = ctx.state.getObject(legend);
        l.zone = ZoneType::LegendZone;
        l.legend_anchor = ctx.source;
        ctx.state.player(ctx.controller).extra_legends.push_back(legend);
        ctx.events.logTrace("NEEKO: " + l.name + " added to the Legend Zone");
    }

private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1085;
        d.def_id = R"RB(rad-167-167)RB";
        d.name = R"RB(Neeko, Blending In)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-167/167)RB";
        d.collector_number = 167;
        d.artist = R"RB(Luscinia Studio)RB";
        d.card_type = CardType::Unit;
        d.super_type = SuperType::Champion;
        d.energy_cost = 6;
        d.power_cost = 1;
        d.might = 4;
        d.rarity = Rarity::Epic;
        d.ability_text = R"RB((Neutral cards can go in decks of any Domain.)
If Neeko is in your deck, choose 3 different legends in addition to your starting legend. (Different means none match names.)
When you play me, choose one of those legends and add it to your Legend Zone while I'm on the board.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/607da6f534ce1b960ad02586ed4cd47c996e2c42-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1085(CardRegistry& r) {
    r.registerCard(1085, std::make_unique<Neeko>());
}

}  // namespace riftbound
