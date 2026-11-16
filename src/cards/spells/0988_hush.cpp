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

class Hush : public SpellCard {
public:
    const CardDef& def() const override { return def_; }
    // "Counter a spell with Power cost [A][A] or more."
    bool hasLegalTargets(const GameState& state, PlayerId /*controller*/) const override {
        for (const auto& it : state.chain.items)
            if (it.is_spell && it.card_def_id != kInvalidId) return true;
        return false;
    }
    void onResolve(CardContext& ctx, const std::vector<GameObjectId>& /*targets*/) override {
        if (ctx.state.chain.items.empty()) return;
        auto& top = ctx.state.chain.items.back();
        if (!top.is_spell || isUncounterable(ctx, top) || top.card_def_id == kInvalidId) return;
        if (ctx.executor.cardDB().get(top.card_def_id).power_cost < 2) return;
        const auto countered = top.source;
        revertCounteredPlay(ctx, top);
        ctx.state.chain.items.pop_back();
        if (ctx.state.objectExists(countered)) {
            ctx.events.logTrace("COUNTER: " + ctx.state.getObject(countered).name + " countered by Hush");
            disposeCounteredCard(ctx, countered);
        }
    }
private:
    const CardDef def_ = [] {
        CardDef d;
        d.id = 988;
        d.def_id = R"RB(rad-041-167)RB";
        d.name = R"RB(Hush)RB";
        d.set_code = R"RB(RAD)RB";
        d.set_name = R"RB(Radiance)RB";
        d.public_code = R"RB(RAD-041/167)RB";
        d.collector_number = 41;
        d.artist = R"RB(Kudos Productions)RB";
        d.card_type = CardType::Spell;
        d.domains = {Domain::Calm};
        d.energy_cost = 1;
        d.power_cost = 2;
        d.rarity = Rarity::Rare;
        d.keywords.set(Keyword::Reaction);
        d.ability_text = R"RB([Reaction] (Play any time, even before spells and abilities resolve.)
Counter a spell with Power cost [A][A] or more.)RB";
        d.image_url = R"RB(https://cmsassets.rgpub.io/sanity/images/dsfx7636/game_data_live/b9c1b9441e42639805b715466938ccd26b89892d-744x1039.png)RB";
        return d;
    }();
};

}  // anonymous namespace

void register_card_988(CardRegistry& r) {
    r.registerCard(988, std::make_unique<Hush>());
}

}  // namespace riftbound
