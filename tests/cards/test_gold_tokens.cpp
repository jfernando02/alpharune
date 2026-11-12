// "Play a Gold gear token exhausted." Honest Broker, Treasure Hunter and Plundering Poro made it
// ready (createGoldToken never exhausted it, and createToken only exhausts units), so the Gold
// could be spent for [A] at once. Found by the card audit (docs/rules-audit/cards.csv).

#include "tests/cards/card_test_fixture.h"

namespace riftbound::test {
namespace {

class GoldTokenTest : public CardTestFixture {
protected:
    GameObjectId onlyGold() {
        GameObjectId g = kInvalidId;
        for (const auto& [id, o] : state.objects)
            if (o.isGear() && o.name == "Gold" && o.location.has_value()) {
                EXPECT_EQ(g, kInvalidId) << "one Gold token";
                g = id;
            }
        return g;
    }
};

TEST_F(GoldTokenTest, GoldFromTriggersEntersExhausted) {
    struct Case { CardDefId id; TriggerType t; };
    for (Case c : {Case{476, TriggerType::WhenIDie}, Case{451, TriggerType::WhenIMove},
                   Case{778, TriggerType::WhenIConquer}}) {
        for (auto it = state.objects.begin(); it != state.objects.end();)
            it = it->second.name == "Gold" ? state.objects.erase(it) : std::next(it);
        auto unit = addUnit(P1, c.id, 0, /*at_bf=*/0);
        EffectExecutor exec(state, events, card_db, &card_registry);
        fireTriggerAs(c.id, P1, unit, c.t, exec);
        auto gold = onlyGold();
        ASSERT_NE(gold, kInvalidId) << card_db.get(c.id).name;
        EXPECT_TRUE(state.getObject(gold).is_exhausted) << card_db.get(c.id).name;
    }
}

}  // namespace
}  // namespace riftbound::test
