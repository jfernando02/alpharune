// CR 319.5: a Cleanup follows the removal of a chain item; CR 323.1: its first task is the win
// check. The chain loop used to resolve every item before any cleanup, so a player who reached
// the Victory Score mid-chain (Ahri, Alluring's point in the Beginning Step) played on.
// Found by riftbound_rules_fuzz (docs/rules-audit/).

#include "tests/cards/card_test_fixture.h"

#include "engine/chain_manager.h"
#include "engine/effect_executor.h"
#include "engine/game_engine.h"

namespace riftbound::test {
namespace {

constexpr CardDefId kConcentrate = 653;  // "Draw 2"

class ChainCleanupTest : public CardTestFixture {};

TEST_F(ChainCleanupTest, CleanupAfterEachItemAndNothingResolvesAfterAWin) {
    for (int i = 0; i < 6; ++i) addToDeck(P1, kInvalidId);
    auto first = addToHand(P1, kConcentrate);
    auto second = addToHand(P1, kConcentrate);
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    cm.setEffectExecutor(&exec);
    int cleanups = 0;
    cm.setCleanup([&] {
        ++cleanups;
        state.game_over = true;  // as if the top item gave the winning point
        state.winner = P1;
    });
    for (auto id : {first, second}) {
        auto& h = state.player(P1).hand;
        h.erase(std::remove(h.begin(), h.end(), id), h.end());
        cm.addSpell(id, P1, {});
    }
    int resolved = 0;
    FirstChoiceAgent agent;
    cm.processFEPR(
        [&](PlayerId, const std::vector<Intent>& legal) { return agent.selectAction(state, legal); },
        [](const ChainItem&) {},
        [&](const ChainItem& item) {
            ++resolved;
            Card* c = card_registry.get(item.card_def_id);
            CardContext ctx{state, events, exec, item.controller, item.source};
            c->onResolve(ctx, item.targets);
        });
    EXPECT_EQ(resolved, 1) << "the game ended after the first item; the second must not resolve";
    EXPECT_GE(cleanups, 1);
}

// CR 309.1: while a Chain exists the turn is Closed. A Cleanup between items that reopened the
// turn (a Showdown ending there) let a spell without [Reaction] be offered at chain priority as
// a normal play, which the priority loop then ignored -- an action that changed nothing, chosen
// again and again (Fading Memories, found by the fuzzer's no-op check).
TEST_F(ChainCleanupTest, TheTurnStaysClosedWhileAChainExists) {
    for (int i = 0; i < 6; ++i) addToDeck(P1, kInvalidId);
    auto first = addToHand(P1, kConcentrate);
    auto second = addToHand(P1, kConcentrate);
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    cm.setEffectExecutor(&exec);
    cm.setCleanup([&] { state.turn.oc_state = OpenClosedState::Open; });
    for (auto id : {first, second}) {
        auto& h = state.player(P1).hand;
        h.erase(std::remove(h.begin(), h.end(), id), h.end());
        cm.addSpell(id, P1, {});
    }
    int priority_checks = 0, open_with_chain = 0;
    FirstChoiceAgent agent;
    cm.processFEPR(
        [&](PlayerId, const std::vector<Intent>& legal) { return agent.selectAction(state, legal); },
        [](const ChainItem&) {},
        [&](const ChainItem& item) {
            Card* c = card_registry.get(item.card_def_id);
            CardContext ctx{state, events, exec, item.controller, item.source};
            c->onResolve(ctx, item.targets);
        },
        [&](PlayerId p) {
            ++priority_checks;
            if (!state.turn.isClosedState()) ++open_with_chain;
            return std::vector<Intent>{Intent::passPriority(p)};
        });
    EXPECT_GE(priority_checks, 2);
    EXPECT_EQ(open_with_chain, 0);
}

// A Showdown that ends while a chain item is still pending (a conquer trigger) leaves the turn
// Closed until the chain is gone (CR 309.1).
TEST_F(ChainCleanupTest, AShowdownEndingWithAChainLeavesTheTurnClosed) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = engine.mutableState();
    BattlefieldState b0; b0.id = 0; s.battlefields.push_back(b0);
    s.turn.phase = TurnPhase::MainPhase;
    s.turn.ns_state = NeutralShowdownState::Showdown;
    auto spell = s.createObject();
    s.getObject(spell).owner = P1; s.getObject(spell).controller = P1;
    ChainItem item; item.id = s.chain.allocateId(); item.source = spell; item.controller = P1;
    s.chain.items.push_back(item);
    engine.testHook_finishShowdown(0);
    EXPECT_TRUE(s.turn.isClosedState());
    s.chain.items.clear();
    engine.testHook_finishShowdown(0);
    EXPECT_FALSE(s.turn.isClosedState());
}

}  // namespace
}  // namespace riftbound::test
