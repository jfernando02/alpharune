/// resumeFromSnapshot at a Showdown focus decision (isCleanShowdownPoint):
/// the look-ahead agents search these decisions by restarting copies there,
/// so a copy must offer exactly the decision the live engine offers, and play
/// on from it.

#include "agents/heuristic_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

class ShowdownResumeTest : public ::testing::Test {
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

    struct Point { GameState state; std::vector<Intent> legal; };

    /// Showdown focus decisions from random games between two decks with
    /// combat tricks.
    static std::vector<Point> showdownPoints(int max_points) {
        const DeckSubmission d1 = DeckValidator::loadFromDeckList(deckPath("leblanc_test.txt"), *db_);
        const DeckSubmission d2 = DeckValidator::loadFromDeckList(deckPath("vex_test_deck.txt"), *db_);
        std::vector<Point> out;
        for (uint64_t seed = 4001; seed <= 4040 && static_cast<int>(out.size()) < max_points; ++seed) {
            EventBus bus;
            GameEngine engine(*db_, bus, *registry_);
            std::mt19937_64 rng(seed * 31);
            StepResult step = engine.beginGame(d1, d2, seed);
            for (int n = 0; n < 4000 && step.kind == StepKind::NeedDecision; ++n) {
                if (isCleanShowdownPoint(engine.state()))
                    out.push_back({engine.state(), step.legal});
                std::uniform_int_distribution<size_t> pick(0, step.legal.size() - 1);
                step = engine.applyChoice(static_cast<int>(pick(rng)));
            }
        }
        return out;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
};

CardRegistry* ShowdownResumeTest::registry_ = nullptr;
CardDB* ShowdownResumeTest::db_ = nullptr;

TEST_F(ShowdownResumeTest, ResumedCopyOffersTheSameDecisionAndPlaysOn) {
    const auto points = showdownPoints(150);
    ASSERT_GE(points.size(), 30u) << "too few Showdown focus decisions to test";
    int combat_points = 0;
    for (size_t k = 0; k < points.size(); ++k) {
        const auto& [state, legal] = points[k];
        // Every focus decision offers the pass.
        EXPECT_TRUE(std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
            return a.type == IntentType::PassFocus;
        })) << "point " << k;
        for (const auto& bf : state.battlefields)
            if (bf.id == *state.turn.awaiting_focus_at && bf.combat_in_progress) ++combat_points;

        EventBus bus;
        GameEngine copy(*db_, bus, *registry_);
        StepResult step = copy.resumeFromSnapshot(state, 77 + k);
        ASSERT_EQ(step.kind, StepKind::NeedDecision) << "point " << k;
        EXPECT_EQ(step.legal, legal) << "point " << k;
        EXPECT_EQ(copy.state().turn.focus_holder, state.turn.focus_holder) << "point " << k;
        EXPECT_EQ(copy.state().turn.players_passed_focus, state.turn.players_passed_focus)
            << "point " << k;

        // Play on to the end of the turn: every choice must be accepted, and
        // the Showdown must close (nothing left in progress or staged).
        std::mt19937_64 rng(k);
        const int turn = state.turn.turn_number;
        for (int n = 0; n < 2000 && step.kind == StepKind::NeedDecision &&
                        copy.state().turn.turn_number == turn;
             ++n) {
            std::uniform_int_distribution<size_t> pick(0, step.legal.size() - 1);
            step = copy.applyChoice(static_cast<int>(pick(rng)));
        }
        if (step.kind == StepKind::NeedDecision) {
            EXPECT_NE(copy.state().turn.turn_number, turn) << "point " << k << ": turn never ended";
            for (const auto& bf : copy.state().battlefields) {
                EXPECT_FALSE(bf.showdown_in_progress) << "point " << k;
                EXPECT_FALSE(bf.combat_in_progress) << "point " << k;
            }
        }
        if (HasFailure()) return;
    }
    EXPECT_GT(combat_points, 0) << "no Combat Showdown step among the points";
}

TEST_F(ShowdownResumeTest, HeuristicSearchesShowdownFocusWhenAsked) {
    // Decisions with a real choice (a lone PassFocus is answered unsearched).
    std::vector<Point> points;
    for (auto& p : showdownPoints(150))
        if (p.legal.size() >= 2 && points.size() < 12) points.push_back(std::move(p));
    ASSERT_GE(points.size(), 5u);
    for (size_t k = 0; k < points.size(); ++k) {
        const auto& [state, legal] = points[k];
        HeuristicConfig off;
        HeuristicAgent plain(*db_, *registry_, off);
        plain.selectAction(state, legal);
        EXPECT_EQ(plain.stats().lookahead_decisions, 0) << "point " << k;

        HeuristicConfig on;
        on.search_showdowns = true;
        on.guesses = 2;
        HeuristicAgent agent(*db_, *registry_, on);
        const Intent pick = agent.selectAction(state, legal);
        EXPECT_NE(std::find(legal.begin(), legal.end(), pick), legal.end()) << "point " << k;
        EXPECT_EQ(agent.stats().showdown_searches, 1) << "point " << k;
        EXPECT_GT(agent.stats().simulations, 0) << "point " << k;
        EXPECT_EQ(agent.stats().failed_simulations, 0) << "point " << k;
    }
}

}  // namespace
}  // namespace riftbound
