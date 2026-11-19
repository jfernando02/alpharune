#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include <memory>
#include <string>
#include <vector>

namespace riftbound {
namespace {

// "Units you play to battlefields you control enter ready."
// Id 1087: The Exile is missing from the card gallery snapshot the Radiance ids
// were ranked from (956 + rank), so it takes the first free id after them. It
// was 1101, past kCardVocabSize, which hid the top meta legend from the models.
class TheExile : public LegendCard {
public:
    const CardDef& def() const override { return def_; }
    bool grantsEnterReadyTo(const GameState& state, GameObjectId /*self*/,
                            const GameObject& played) const override {
        if (!played.isUnit() || !played.location.has_value()) return false;
        const auto* at = std::get_if<BattlefieldLocation>(&*played.location);
        if (!at) return false;
        for (const auto& bf : state.battlefields)
            if (bf.id == at->id)
                return bf.controller.has_value() && *bf.controller == played.controller;
        return false;
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 1087;
        d.def_id = R"RB(rad-149-167)RB";
        d.name = R"RB(The Exile)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-149/167)RB";
        d.collector_number = 149;
        d.artist = R"RB(Grafit Studio)RB";
        d.card_type = CardType::Legend;
        d.domains = {Domain::Calm, Domain::Chaos};
        d.tags = {R"RB(Riven)RB"};
        d.rarity = Rarity::Rare;
        d.ability_text = R"RB(Units you play to battlefields you control enter ready.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/342befcaab98642a2348bd2f6fdb92c68563b6d7-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_1087(CardRegistry& r) {
    r.registerCard(1087, std::make_unique<TheExile>());
}

} // namespace riftbound
