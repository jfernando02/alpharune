// CR 319.5: a Cleanup follows the removal of a chain item; CR 323.1: its first task is the win
// check. The chain loop used to resolve every item before any cleanup, so a player who reached
// the Victory Score mid-chain (Ahri, Alluring's point in the Beginning Step) played on.
// Found by riftbound_rules_fuzz (docs/rules-audit/).

#include "tests/cards/card_test_fixture.h"

#include "engine/chain_manager.h"
#include "engine/effect_executor.h"

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

}  // namespace
}  // namespace riftbound::test
