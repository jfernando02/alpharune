/// CR 323.10 (190.3.b.1): Contested status is removed in the Cleanup that
/// follows the contester's last Unit leaving the battlefield. A main-phase
/// priority decision must never be offered with a battlefield still contested
/// by a player who has no Units there — otherwise a copy restarted with
/// `resumeFromSnapshot` (which re-runs that Cleanup) differs from the live
/// state it was copied from.

#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

class ContestedCleanupTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        registry_ = new CardRegistry();
        registry_->loadAll();
        db_ = new CardDB();
        db_->buildFromClasses(*registry_);
    }
    static void TearDownTestSuite() {
        delete db_;
        delete registry_;
        db_ = nullptr;
        registry_ = nullptr;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
};

CardRegistry* ContestedCleanupTest::registry_ = nullptr;
CardDB* ContestedCleanupTest::db_ = nullptr;

/// The turn player's main-phase priority decision: the only decision that
/// offers EndTurn, taken Open / Neutral with nothing on the chain, no cost
/// being paid and no Showdown or Combat staged or underway.
bool isMainPriorityDecision(const GameState& gs, const std::vector<Intent>& legal) {
    if (gs.game_over || gs.turn.phase != TurnPhase::MainPhase ||
        !gs.turn.isNeutralOpen() || gs.chain.exists() || gs.cost_cursor.has_value())
        return false;
    if (std::none_of(legal.begin(), legal.end(),
                     [](const Intent& a) { return a.type == IntentType::EndTurn; }))
        return false;
    for (const auto& bf : gs.battlefields) {
        if (bf.combat_in_progress || bf.showdown_in_progress ||
            bf.combat_staged || bf.showdown_staged)
            return false;
    }
    return true;
}

std::string describe(const BattlefieldState& bf) {
    std::ostringstream os;
    os << "BF#" << bf.id << " contested=" << bf.is_contested
       << " contested_by=" << toString(bf.contested_by) << " controller="
       << (bf.controller ? toString(*bf.controller) : "none")
       << " showdown_staged=" << bf.showdown_staged
       << " combat_staged=" << bf.combat_staged;
    return os.str();
}

// Random games between the decks the stale flag was first seen with. At every
// main-phase priority decision: no battlefield may be contested by a player
// without Units there, and a copy resumed from the state must reach the same
// battlefield status (resume re-runs Cleanup before offering the decision).
TEST_F(ContestedCleanupTest, NoStaleContestedAtMainPriorityDecision) {
    const DeckSubmission deck1 = DeckValidator::loadFromDeckList(deckPath("leblanc_test.txt"), *db_);
    const DeckSubmission deck2 = DeckValidator::loadFromDeckList(deckPath("vex_test_deck.txt"), *db_);

    int priority_decisions = 0;
    for (uint64_t seed = 3001; seed <= 3012; ++seed) {
        EventBus bus;
        GameEngine engine(*db_, bus, *registry_);
        std::mt19937_64 pick_rng(seed * 7919);
        StepResult step = engine.beginGame(deck1, deck2, seed);

        for (int n = 0; n < 4000 && step.kind == StepKind::NeedDecision; ++n) {
            const GameState& gs = engine.state();
            if (isMainPriorityDecision(gs, step.legal)) {
                ++priority_decisions;
                for (const auto& bf : gs.battlefields) {
                    if (!bf.is_contested) continue;
                    if (bf.contested_by == PlayerId::None) continue;
                    EXPECT_TRUE(bf.hasUnitsFrom(bf.contested_by, gs.objects))
                        << "seed " << seed << " turn " << gs.turn.turn_number
                        << " decision " << n << ": " << describe(bf)
                        << " — contester has no units there";
                }

                EventBus copy_bus;
                GameEngine copy(*db_, copy_bus, *registry_);
                copy.resumeFromSnapshot(gs, seed + 1);
                const auto& resumed = copy.state().battlefields;
                ASSERT_EQ(resumed.size(), gs.battlefields.size());
                for (size_t i = 0; i < resumed.size(); ++i) {
                    EXPECT_EQ(resumed[i].is_contested, gs.battlefields[i].is_contested)
                        << "seed " << seed << " turn " << gs.turn.turn_number
                        << " decision " << n << "\n  live:    " << describe(gs.battlefields[i])
                        << "\n  resumed: " << describe(resumed[i]);
                    EXPECT_EQ(resumed[i].contested_by, gs.battlefields[i].contested_by)
                        << "seed " << seed << " decision " << n;
                }
                if (HasFailure()) return;  // one report is enough
            }
            std::uniform_int_distribution<size_t> pick(0, step.legal.size() - 1);
            step = engine.applyChoice(static_cast<int>(pick(pick_rng)));
        }
    }
    EXPECT_GT(priority_decisions, 100);
}

}  // namespace
}  // namespace riftbound
