/// resumeFromSnapshot at a chain priority decision (isCleanChainPoint) and at
/// the setup decisions (isCleanSetupPoint): the look-ahead agents search
/// these decisions by restarting copies there, so a copy must offer the
/// decision the live engine offers and then go on as the live engine does —
/// through the rest of the chain and the step it ran in.

#include "agents/heuristic_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

std::string show(const std::vector<Intent>& legal) {
    std::string out;
    for (const auto& a : legal) out += "\n    " + a.describe();
    return out;
}

class ChainResumeTest : public ::testing::Test {
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

    struct Lockstep {
        int points = 0;       // snapshots resumed
        int steps = 0;        // decisions compared after them
    };

    /// Random games between two decks with combat tricks. At each decision
    /// `take` accepts, a copy is resumed from the snapshot and both engines
    /// are fed the same random choices until the live one reaches its next
    /// main-phase or Showdown focus decision (or the turn ends): every
    /// decision the copy offers must equal the live one. At most `max_points`.
    static Lockstep runLockstep(const std::function<bool(const GameState&)>& take,
                                int max_points, std::vector<Point>* taken = nullptr) {
        const DeckSubmission d1 = DeckValidator::loadFromDeckList(deckPath("leblanc_test.txt"), *db_);
        const DeckSubmission d2 = DeckValidator::loadFromDeckList(deckPath("vex_test_deck.txt"), *db_);
        Lockstep out;
        for (uint64_t seed = 5001; seed <= 5060 && out.points < max_points; ++seed) {
            EventBus bus;
            GameEngine live(*db_, bus, *registry_);
            std::mt19937_64 rng(seed * 17);
            StepResult step = live.beginGame(d1, d2, seed);
            for (int n = 0; n < 4000 && step.kind == StepKind::NeedDecision; ++n) {
                auto pick = [&](size_t size) {
                    return static_cast<int>(std::uniform_int_distribution<size_t>(0, size - 1)(rng));
                };
                if (out.points >= max_points || !take(live.state())) {
                    step = live.applyChoice(pick(step.legal.size()));
                    continue;
                }
                ++out.points;
                if (taken) taken->push_back({live.state(), step.legal});
                EventBus copy_bus;
                GameEngine copy(*db_, copy_bus, *registry_);
                StepResult cstep = copy.resumeFromSnapshot(live.state(), seed);
                const int turn = live.state().turn.turn_number;
                const ChainStep taken_step = live.state().turn.chain_step;
                for (int s = 0; s < 200; ++s) {
                    EXPECT_EQ(cstep.kind, step.kind) << "seed " << seed << " step " << s;
                    if (cstep.kind != step.kind || step.kind != StepKind::NeedDecision) return out;
                    EXPECT_TRUE(cstep.legal == step.legal)
                        << "seed " << seed << " step " << s << " (phase "
                        << toString(live.state().turn.phase) << ", chain "
                        << live.state().chain.items.size() << ", resolving "
                        << (live.state().chain.resuming
                                ? live.state().getObject(live.state().chain.resuming->source).name
                                : std::string("-"))
                        << "; copy chain " << copy.state().chain.items.size() << ", turn "
                        << copy.state().turn.turn_number << " vs " << turn << ", start chain_step "
                        << static_cast<int>(taken_step) << ")\n  copy:" << show(cstep.legal)
                        << "\n  live:" << show(step.legal);
                    if (cstep.legal != step.legal) return out;
                    ++out.steps;
                    const int i = pick(step.legal.size());
                    step = live.applyChoice(i);
                    cstep = copy.applyChoice(i);
                    const GameState& ls = live.state();
                    if (isCleanResumePoint(ls) || isCleanShowdownPoint(ls) ||
                        isCleanSetupPoint(ls) || ls.turn.turn_number != turn)
                        break;
                }
                if (step.kind == StepKind::NeedDecision) {
                    EXPECT_TRUE(cstep.legal == step.legal)
                        << "seed " << seed << " at the stop\n  copy:" << show(cstep.legal)
                        << "\n  live:" << show(step.legal);
                }
                if (::testing::Test::HasFailure()) return out;
            }
        }
        return out;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
};

CardRegistry* ChainResumeTest::registry_ = nullptr;
CardDB* ChainResumeTest::db_ = nullptr;

TEST_F(ChainResumeTest, ChainPriorityCopyGoesOnAsTheLiveEngine) {
    int main_steps = 0, showdown_steps = 0;
    const auto run = runLockstep(
        [&](const GameState& s) {
            if (!isCleanChainPoint(s)) return false;
            (s.turn.chain_step == ChainStep::ShowdownAction ? showdown_steps : main_steps)++;
            return true;
        },
        200);
    ASSERT_GE(run.points, 40) << "too few chain priority decisions to test";
    EXPECT_GT(run.steps, run.points);
    EXPECT_GT(main_steps, 0) << "no chain in a main-phase action";
    EXPECT_GT(showdown_steps, 0) << "no chain in a Showdown focus action";
}

TEST_F(ChainResumeTest, SetupCopyGoesOnAsTheLiveEngine) {
    int picks = 0, mulligans = 0;
    const auto run = runLockstep(
        [&](const GameState& s) {
            if (!isCleanSetupPoint(s)) return false;
            (s.turn.phase == TurnPhase::Setup ? picks : mulligans)++;
            return true;
        },
        40);
    ASSERT_EQ(run.points, picks + mulligans);
    EXPECT_GE(picks, 10);
    EXPECT_GE(mulligans, 10);
}

TEST_F(ChainResumeTest, ChainPointsNeedAKnownStep) {
    GameState s;
    s.turn.phase = TurnPhase::MainPhase;
    s.turn.priority_holder = PlayerId::Player1;
    s.chain.awaiting_priority = true;
    ChainItem item;
    item.status = ChainItemStatus::Finalized;
    s.chain.items.push_back(item);
    EXPECT_FALSE(isCleanChainPoint(s)) << "a chain outside any known step";
    s.turn.chain_step = ChainStep::MainAction;
    EXPECT_TRUE(isCleanChainPoint(s));
    s.chain.items[0].status = ChainItemStatus::Pending;
    EXPECT_FALSE(isCleanChainPoint(s)) << "an item not yet finalized";
    s.chain.items[0].status = ChainItemStatus::Finalized;
    s.chain.awaiting_priority = false;
    EXPECT_FALSE(isCleanChainPoint(s)) << "not at a priority decision";
}

TEST_F(ChainResumeTest, HeuristicSearchesChainAndSetupWhenAsked) {
    std::vector<Point> chain_points, setup_points;
    runLockstep([](const GameState& s) { return isCleanChainPoint(s); }, 200, &chain_points);
    runLockstep([](const GameState& s) { return isCleanSetupPoint(s); }, 12, &setup_points);
    std::vector<Point> points;
    for (auto* from : {&chain_points, &setup_points}) {
        int n = 0;
        for (auto& p : *from)
            if (p.legal.size() >= 2 && n < 6) {
                points.push_back(p);
                ++n;
            }
    }
    ASSERT_GE(points.size(), 8u);
    for (size_t k = 0; k < points.size(); ++k) {
        const auto& [state, legal] = points[k];
        const bool setup = isCleanSetupPoint(state);
        HeuristicAgent plain(*db_, *registry_, HeuristicConfig{});
        plain.selectAction(state, legal);
        EXPECT_EQ(plain.stats().lookahead_decisions, 0) << "point " << k;

        HeuristicConfig on;
        on.search_chain = true;
        on.search_battlefield = true;
        on.search_mulligan = true;
        on.guesses = 2;
        on.setup_guesses = 2;
        HeuristicAgent agent(*db_, *registry_, on);
        const Intent pick = agent.selectAction(state, legal);
        EXPECT_NE(std::find(legal.begin(), legal.end(), pick), legal.end()) << "point " << k;
        EXPECT_EQ(agent.stats().chain_searches, setup ? 0 : 1) << "point " << k;
        EXPECT_EQ(agent.stats().setup_searches, setup ? 1 : 0) << "point " << k;
        EXPECT_GT(agent.stats().simulations, 0) << "point " << k;
        EXPECT_EQ(agent.stats().failed_simulations, 0) << "point " << k;
    }
}

}  // namespace
}  // namespace riftbound
