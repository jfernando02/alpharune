// Every card is in exactly one zone. Found by riftbound_rules_fuzz (docs/rules-audit/):
// a recycled card stayed in the list it came from (Fizz, Trickster and Kai'Sa, Evolutionary's
// "then recycle it" left the spell in the trash AND the deck: a free extra copy), a recycled
// token went into the deck instead of ceasing to exist, and a spell that banishes itself while
// resolving (Time Warp, Arcane Shift) was also put into the trash.

#include "tests/cards/card_test_fixture.h"

#include "engine/effect_executor.h"

#include <algorithm>

namespace riftbound::test {
namespace {

constexpr CardDefId kTimeWarp = 122;
constexpr CardDefId kConcentrate = 653;  // "Draw 2": a spell with no targets
constexpr CardDefId kKeepersVerdict = 760;

class ZoneIntegrityTest : public CardTestFixture {
protected:
    long listed(GameObjectId id) {
        long n = 0;
        for (const auto& ps : state.players)
            for (const auto* v : {&ps.hand, &ps.main_deck, &ps.rune_deck, &ps.trash, &ps.banishment})
                n += std::count(v->begin(), v->end(), id);
        return n;
    }
    GameObjectId toTrash(GameObjectId id, PlayerId p) {
        auto& ps = state.player(p);
        ps.main_deck.erase(std::remove(ps.main_deck.begin(), ps.main_deck.end(), id), ps.main_deck.end());
        ps.trash.push_back(id);
        state.getObject(id).zone = ZoneType::Trash;
        return id;
    }
};

TEST_F(ZoneIntegrityTest, RecyclingTakesTheCardOutOfItsList) {
    auto card = toTrash(addToDeck(P1, kConcentrate), P1);
    EffectExecutor exec(state, events, card_db, &card_registry);
    exec.recycleCards(P1, {card});
    EXPECT_TRUE(inDeck(P1, card));
    EXPECT_FALSE(inTrash(P1, card));
    EXPECT_EQ(listed(card), 1);
}

TEST_F(ZoneIntegrityTest, RecycledTokenCeasesToExist) {
    auto token = addUnit(P1, kInvalidId, 1);
    state.getObject(token).is_token = true;
    const auto deck_before = deckSize(P1);
    EffectExecutor exec(state, events, card_db, &card_registry);
    exec.recycleCards(P1, {token});
    EXPECT_EQ(deckSize(P1), deck_before) << "a token routed to a non-board zone ceases to exist (CR 183.1)";
    EXPECT_EQ(listed(token), 0);
    EXPECT_FALSE(state.getObject(token).location.has_value());
    EXPECT_NE(state.getObject(token).zone, ZoneType::MainDeck);
}

TEST_F(ZoneIntegrityTest, SelfBanishingSpellIsNotAlsoTrashed) {
    auto spell = addToHand(P1, kTimeWarp);
    EffectExecutor exec(state, events, card_db, &card_registry);
    FirstChoiceAgent agent;
    driveThroughChain(spell, P1, {}, agent, exec);
    const auto& b = state.player(P1).banishment;
    EXPECT_NE(std::find(b.begin(), b.end(), spell), b.end()) << "Time Warp: banish this";
    EXPECT_FALSE(inTrash(P1, spell));
    EXPECT_EQ(listed(spell), 1);
    EXPECT_EQ(state.getObject(spell).zone, ZoneType::Banishment);
}

// "Then recycle it" (Fizz, Trickster; Kai'Sa, Evolutionary) replaces where the spell goes as it
// leaves the chain: the bottom of the deck, and nowhere else.
TEST_F(ZoneIntegrityTest, ThenRecycleItPutsTheSpellOnlyInTheDeck) {
    for (int i = 0; i < 3; ++i) addToDeck(P1, kInvalidId);  // something to draw
    auto spell = addToHand(P1, kConcentrate);
    state.getObject(spell).card_counters["__recycle_on_leave_chain"] = 1;
    EffectExecutor exec(state, events, card_db, &card_registry);
    FirstChoiceAgent agent;
    driveThroughChain(spell, P1, {}, agent, exec);
    EXPECT_TRUE(inDeck(P1, spell));
    EXPECT_FALSE(inTrash(P1, spell));
    EXPECT_EQ(listed(spell), 1);
}

// Keeper's Verdict on a token: "its owner places it on the top or bottom of their Main Deck"
// sends a token to a non-board zone, so it ceases to exist instead of becoming a deck card.
TEST_F(ZoneIntegrityTest, KeepersVerdictOnATokenRemovesIt) {
    auto token = addUnit(P2, kInvalidId, 1, /*at_bf=*/0);
    state.getObject(token).is_token = true;
    const auto deck_before = deckSize(P2);
    auto spell = addToHand(P1, kKeepersVerdict);
    EffectExecutor exec(state, events, card_db, &card_registry);
    FirstChoiceAgent agent;
    driveThroughChain(spell, P1, {token}, agent, exec);
    EXPECT_EQ(deckSize(P2), deck_before);
    EXPECT_EQ(listed(token), 0);
    EXPECT_FALSE(state.getObject(token).location.has_value()) << "it left the board";
}

// A move goes from one board location to another. Azir, Ascendant killed in response to his own
// swap ability was "moved" back to his base out of the trash (found by the fuzzer, seed 5).
TEST_F(ZoneIntegrityTest, MovingAnObjectThatLeftTheBoardDoesNothing) {
    auto unit = addUnit(P1, kInvalidId, 2, /*at_bf=*/0);
    auto& u = state.getObject(unit);
    u.location.reset();
    u.zone = ZoneType::Trash;
    state.player(P1).trash.push_back(unit);
    EffectExecutor exec(state, events, card_db, &card_registry);
    exec.moveToBase(unit);
    exec.moveToBattlefield(unit, 1);
    EXPECT_EQ(state.getObject(unit).zone, ZoneType::Trash);
    EXPECT_FALSE(state.getObject(unit).location.has_value());
    EXPECT_EQ(listed(unit), 1);
}

}  // namespace
}  // namespace riftbound::test
