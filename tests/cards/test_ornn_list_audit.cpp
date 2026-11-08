// Ornn (Vendetta) list audit against the official card text: Aspiring
// Engineer, Ornn Blacksmith, Back Off, Defy, Eclipse.
#include "tests/cards/card_test_fixture.h"
#include "engine/effect_executor.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace riftbound::test {
namespace {

constexpr CardDefId kDefy = 45;               // counter a spell: <= [4] and <= [A]
constexpr CardDefId kDisintegrate = 5;        // spell [4]
constexpr CardDefId kFallingStar = 29;        // spell [2] + 2 Power
constexpr CardDefId kOrnnBlacksmith = 381;
constexpr CardDefId kAspiringEngineer = 384;  // "return a gear from your trash to your hand"
constexpr CardDefId kBrutalizer = 365;
constexpr CardDefId kBackOff = 604;           // "If you played this from your hand, draw 1."
constexpr CardDefId kEclipse = 625;           // -4 [M], [Predict]

class OrnnListAuditTest : public CardTestFixture {
protected:
    GameObjectId gearAtBase(PlayerId p, CardDefId def_id) {
        auto id = state.createObject();
        auto& g = state.getObject(id);
        const auto& def = card_db.get(def_id);
        g.owner = g.controller = p;
        g.card_def_id = def_id;
        g.card_type = CardType::Gear;
        g.name = def.name;
        g.zone = ZoneType::Base;
        g.location = BaseLocation{p};
        return id;
    }
    GameObjectId trashCard(PlayerId p, CardDefId def_id) {
        auto id = state.createObject();
        auto& c = state.getObject(id);
        const auto& def = card_db.get(def_id);
        c.owner = c.controller = p;
        c.card_def_id = def_id;
        c.card_type = def.card_type;
        c.name = def.name;
        c.zone = ZoneType::Trash;
        state.player(p).trash.push_back(id);
        return id;
    }
    GameObjectId deckCard(PlayerId p, CardDefId def_id) {
        auto id = addToDeck(p, def_id);
        if (def_id != kInvalidId) state.getObject(id).card_type = card_db.get(def_id).card_type;
        return id;
    }
};

TEST_F(OrnnListAuditTest, AspiringEngineer_ReturnsAGearFromTheTrashNotFromTheBoard) {
    EffectExecutor exec(state, events, card_db);
    auto on_board = gearAtBase(P1, kBrutalizer);
    auto trashed = trashCard(P1, kBrutalizer);
    auto eng = addUnit(P1, kAspiringEngineer);
    fireTriggerAs(kAspiringEngineer, P1, eng, TriggerType::WhenYouPlayMe, exec, {on_board});
    EXPECT_TRUE(state.getObject(on_board).location.has_value()) << "the gear in play stays";
    EXPECT_TRUE(inHand(P1, trashed));
    EXPECT_TRUE(state.player(P1).trash.empty());
}

TEST_F(OrnnListAuditTest, AspiringEngineer_NothingInTheTrashDoesNothing) {
    EffectExecutor exec(state, events, card_db);
    auto on_board = gearAtBase(P1, kBrutalizer);
    auto eng = addUnit(P1, kAspiringEngineer);
    fireTriggerAs(kAspiringEngineer, P1, eng, TriggerType::WhenYouPlayMe, exec, {on_board});
    EXPECT_TRUE(state.getObject(on_board).location.has_value());
    EXPECT_TRUE(state.player(P1).hand.empty());
}

TEST_F(OrnnListAuditTest, OrnnBlacksmith_TheAgentPicksWhichGearToDraw) {
    EffectExecutor exec(state, events, card_db);
    // Deck bottom -> top: filler, gear A, filler, gear B (top).
    deckCard(P1, kInvalidId);
    auto gear_a = deckCard(P1, kBrutalizer);
    deckCard(P1, kInvalidId);
    auto gear_b = deckCard(P1, kBrutalizer);
    auto ornn = addUnit(P1, kOrnnBlacksmith);
    std::vector<GameObjectId> offered;
    driveResumableTrigger(kOrnnBlacksmith, P1, ornn,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal)
                for (auto c : i.chosen_objects) offered.push_back(c);
            return legal.back();
        },
        exec, TriggerType::WhenYouPlayMe);
    EXPECT_EQ(offered.size(), 2u) << "both gears among the top 4 are offered";
    const bool a_in_hand = inHand(P1, gear_a), b_in_hand = inHand(P1, gear_b);
    EXPECT_NE(a_in_hand, b_in_hand) << "exactly one gear is drawn";
    EXPECT_EQ(state.player(P1).hand.size(), 1u);
    EXPECT_EQ(state.player(P1).main_deck.size(), 3u) << "the other three are recycled";
}

TEST_F(OrnnListAuditTest, BackOff_DrawsOnlyWhenPlayedFromHand) {
    EffectExecutor exec(state, events, card_db);
    auto target = addUnit(P2, kInvalidId, 3, 0);
    for (int i = 0; i < 3; ++i) deckCard(P1, kInvalidId);

    auto from_hand = addToHand(P1, kBackOff);
    state.player(P1).hand.clear();               // it is on the chain now
    invokeOnResolve(from_hand, kBackOff, P1, {target}, exec);
    EXPECT_EQ(state.player(P1).hand.size(), 1u) << "played from hand: draw 1";

    auto from_facedown = addToHand(P1, kBackOff);
    state.player(P1).hand.erase(std::remove(state.player(P1).hand.begin(),
                                            state.player(P1).hand.end(), from_facedown),
                                state.player(P1).hand.end());
    state.getObject(from_facedown).card_counters["__played_not_from_hand_turn"] =
        state.turn.turn_number + 1;              // what TriggerManager marks
    invokeOnResolve(from_facedown, kBackOff, P1, {target}, exec);
    EXPECT_EQ(state.player(P1).hand.size(), 1u) << "played from face down: no draw";
}

TEST_F(OrnnListAuditTest, Defy_CantCounterASpellCostingMoreThanOnePower) {
    EffectExecutor exec(state, events, card_db);
    pushSpellOnChain(P2, kFallingStar);          // [2] + 2 Power
    auto defy = addToHand(P1, kDefy);
    invokeOnResolve(defy, kDefy, P1, {}, exec);
    ASSERT_EQ(state.chain.items.size(), 1u) << "2 Power is more than [A]";

    state.chain.items.clear();
    pushSpellOnChain(P2, kDisintegrate);         // [4]
    invokeOnResolve(defy, kDefy, P1, {}, exec);
    EXPECT_TRUE(state.chain.items.empty()) << "[4] and no Power is counterable";
}

TEST_F(OrnnListAuditTest, Eclipse_Predicts) {
    EffectExecutor exec(state, events, card_db);
    auto unit = addUnit(P2, kInvalidId, 6, 0);
    deckCard(P1, kInvalidId);
    deckCard(P1, kInvalidId);
    int predicted = 0;
    auto conn = events.on_card_revealed.connect([&](const CardRevealedEvent&) { ++predicted; });
    auto spell = addToHand(P1, kEclipse);
    invokeOnResolve(spell, kEclipse, P1, {unit}, exec);
    EXPECT_EQ(state.getObject(unit).temp_might_bonus, -4);
    EXPECT_GE(predicted, 1) << "the top card was looked at";
    conn.disconnect();
}

}  // namespace
}  // namespace riftbound::test
