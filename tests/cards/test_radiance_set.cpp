/// @file test_radiance_set.cpp
/// The rest of the Radiance set: the Bomb token and Deploy, gear Deathknell,
/// and a sample of the new cards.

#include "tests/cards/card_test_fixture.h"

#include "cards/card.h"
#include "cards/card_helpers.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/chain_manager.h"
#include "engine/effect_executor.h"
#include "engine/trigger_manager.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace riftbound::test {
namespace {

constexpr CardDefId kBomb              = 1086;
constexpr CardDefId kHoldEm            = 1010;
constexpr CardDefId kAvalanche         = 1018;
constexpr CardDefId kHush              = 988;
constexpr CardDefId kPartyCrasher      = 1049;
constexpr CardDefId kRousingDisplay    = 1052;
constexpr CardDefId kBattlefieldArmorer = 1054;
constexpr CardDefId kMechanicalBandit  = 1039;

class RadianceSet : public CardTestFixture {
protected:
    int recruits(PlayerId p) {
        int n = 0;
        for (const auto& [id, o] : state.objects)
            if (o.isUnit() && o.controller == p && o.location.has_value() && o.name == "Recruit") ++n;
        return n;
    }
    CardDefId findSpell(int min_power, int max_power) {
        for (CardDefId cid : card_registry.classDefIds()) if (const Card* c = card_registry.get(cid)) {
            const auto& d = c->def();
            if (d.card_type == CardType::Spell && d.power_cost >= min_power && d.power_cost <= max_power)
                return d.id;
        }
        return kInvalidId;
    }
};

TEST_F(RadianceSet, EveryRadianceCardIsRegistered) {
    int n = 0;
    for (CardDefId id = 956; id <= 1086; ++id) {
        const Card* c = card_registry.get(id);
        if (c && c->def().set_code == "RAD") ++n;
    }
    EXPECT_GE(n, 120);
    ASSERT_NE(card_registry.get(kBomb), nullptr);
    EXPECT_TRUE(card_registry.get(kBomb)->def().deploy);
}

// A Bomb dying puts its Deathknell on the chain; it deals 2 to an enemy unit
// there and kills the controller's other Bombs there.
TEST_F(RadianceSet, BombDeathknell_DealsTwoAndKillsOtherBombsHere) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();

    auto src = addUnit(P1, kInvalidId, 1, 0);
    CardContext ctx{state, events, exec, P1, src};
    const auto bomb = createBomb(ctx, 0, P1);
    const auto other = createBomb(ctx, 0, P1);
    const auto elsewhere = createBomb(ctx, 1, P1);
    ASSERT_TRUE(state.objectExists(bomb));
    EXPECT_TRUE(state.getObject(bomb).deploy);
    const auto enemy = addUnit(P2, kInvalidId, 3, 0);

    exec.killObject(bomb);
    ASSERT_EQ(state.chain.items.size(), 1u) << "the Bomb's Deathknell fires";
    state.chain.items.clear();

    driveResumableTrigger(kBomb, P1, bomb, nullptr, exec, TriggerType::WhenIDie);
    EXPECT_EQ(state.getObject(enemy).damage_marked, 2);
    EXPECT_FALSE(state.getObject(other).location.has_value()) << "the other Bomb here dies";
    ASSERT_TRUE(state.objectExists(elsewhere));
    EXPECT_TRUE(state.getObject(elsewhere).location.has_value()) << "a Bomb elsewhere stays";
}

TEST_F(RadianceSet, FriendlyGearDyingAtABattlefieldFiresWatchers) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    addUnit(P1, kBattlefieldArmorer, 3, 1);
    auto gear = addUnit(P1, kInvalidId, 0, 0);
    state.getObject(gear).card_type = CardType::Gear;
    exec.killObject(gear);
    ASSERT_EQ(state.chain.items.size(), 1u);
    EXPECT_EQ(state.chain.items.back().card_def_id, kBattlefieldArmorer);
    EXPECT_EQ(state.chain.items.back().triggering_subject, gear);
}

TEST_F(RadianceSet, HoldEm_CheaperAfterPlayingToABattlefield) {
    const Card* c = card_registry.get(kHoldEm);
    EXPECT_EQ(c->selfCostReduction(state, P1), 0);
    state.player(P1).played_permanent_to_bf_this_turn = true;
    EXPECT_EQ(c->selfCostReduction(state, P1), 2);
    state.player(P1).resetTurnTracking();
    EXPECT_EQ(c->selfCostReduction(state, P1), 0);
}

TEST_F(RadianceSet, PartyCrasher_CostsOneLessPerExhaustedUnit) {
    const Card* c = card_registry.get(kPartyCrasher);
    auto a = addUnit(P1, kInvalidId, 2, 0);
    addUnit(P1, kInvalidId, 2, 0);
    EXPECT_EQ(c->selfCostReduction(state, P1), 0);
    state.getObject(a).is_exhausted = true;
    EXPECT_EQ(c->selfCostReduction(state, P1), 1);
}

TEST_F(RadianceSet, Avalanche_HitsOnlyUnitsInBases) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto base_mine = addUnit(P1, kInvalidId, 5);
    auto base_theirs = addUnit(P2, kInvalidId, 2);
    auto at_bf = addUnit(P2, kInvalidId, 2, 0);
    auto src = pushSpellOnChain(P1, kAvalanche);
    state.chain.items.clear();
    invokeOnResolve(src, kAvalanche, P1, {}, exec);
    EXPECT_EQ(state.getObject(base_mine).damage_marked, 3);
    EXPECT_FALSE(state.getObject(base_theirs).location.has_value()) << "2-Might unit dies";
    EXPECT_EQ(state.getObject(at_bf).damage_marked, 0);
}

TEST_F(RadianceSet, Hush_CountersOnlySpellsWithTwoOrMorePower) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const CardDefId big = findSpell(2, 9), small = findSpell(0, 1);
    ASSERT_NE(big, kInvalidId);
    ASSERT_NE(small, kInvalidId);

    pushSpellOnChain(P2, small);
    auto hush = state.createObject();
    invokeOnResolve(hush, kHush, P1, {}, exec);
    EXPECT_EQ(state.chain.items.size(), 1u) << "a cheap spell is not countered";

    state.chain.items.clear();
    pushSpellOnChain(P2, big);
    invokeOnResolve(hush, kHush, P1, {}, exec);
    EXPECT_TRUE(state.chain.items.empty()) << "a [A][A] spell is countered";
}

TEST_F(RadianceSet, RousingDisplay_FourRecruitsWhenItCanShowOff) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto src = state.createObject();
    ChainItem ri;
    state.chain.resuming = ri;
    invokeOnResolve(src, kRousingDisplay, P1, {}, exec);
    EXPECT_EQ(recruits(P1), 2);

    CardDefId seven = kInvalidId;
    for (CardDefId cid : card_registry.classDefIds()) if (const Card* c = card_registry.get(cid))
        if (c->def().card_type == CardType::Unit && c->def().energy_cost >= 7) { seven = c->def().id; break; }
    ASSERT_NE(seven, kInvalidId);
    addToHand(P1, seven);
    invokeOnResolve(src, kRousingDisplay, P1, {}, exec);
    EXPECT_EQ(recruits(P1), 2 + 4);
    state.chain.resuming.reset();
}

TEST_F(RadianceSet, MechanicalBandit_DrawsWhenItRevealsTheCheaperCard) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    CardDefId cheap = kInvalidId, dear = kInvalidId;
    for (CardDefId cid : card_registry.classDefIds()) if (const Card* c = card_registry.get(cid)) {
        const auto& d = c->def();
        if (d.card_type != CardType::Unit) continue;
        if (cheap == kInvalidId && d.energy_cost == 1) cheap = d.id;
        if (dear == kInvalidId && d.energy_cost >= 6) dear = d.id;
    }
    ASSERT_NE(cheap, kInvalidId);
    ASSERT_NE(dear, kInvalidId);
    auto bandit = addUnit(P1, kMechanicalBandit, 0, 0);
    addToDeck(P1, dear);
    auto mine = addToDeck(P1, cheap);
    auto theirs = addToDeck(P2, dear);
    fireTriggerAs(kMechanicalBandit, P1, bandit, TriggerType::AtStartOfBeginning, exec);
    const auto& hand = state.player(P1).hand;
    EXPECT_NE(std::find(hand.begin(), hand.end(), mine), hand.end()) << "drew the cheaper card";
    EXPECT_EQ(state.player(P2).main_deck.front(), theirs) << "the opponent's card is recycled";
}

}  // namespace
}  // namespace riftbound::test
