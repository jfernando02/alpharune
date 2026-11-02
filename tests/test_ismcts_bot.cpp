/// Tests for IsmctsBot, the engine-native port of OpenSpiel's ISMCTSBot.
/// Positions come from real scripted-vs-scripted games, as in
/// test_heuristic_agent.cpp.

#include "agents/heuristic_agent.h"
#include "agents/ismcts_bot.h"
#include "agents/random_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

class IsmctsBotTest : public ::testing::Test {
protected:
    CardRegistry card_registry;
    CardDB card_db;
    DeckSubmission deck1, deck2;

    void SetUp() override {
        card_registry.loadAll();
        card_db.buildFromClasses(card_registry);
        deck1 = DeckValidator::loadFromDeckList(deckPath("leblanc_test.txt"), card_db);
        deck2 = DeckValidator::loadFromDeckList(deckPath("vex_test_deck.txt"), card_db);
    }

    /// Up to `count` searchable decisions from a scripted-vs-scripted game.
    std::vector<std::pair<GameState, std::vector<Intent>>> lookaheadPoints(uint64_t seed,
                                                                          int count) {
        std::vector<std::pair<GameState, std::vector<Intent>>> out;
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        ScriptedAgent scripted(card_db);
        StepResult step = engine.beginGame(deck1, deck2, seed);
        for (int s = 0; s < 2000 && step.kind == StepKind::NeedDecision &&
                        static_cast<int>(out.size()) < count;
             ++s) {
            if (step.legal.size() >= 2 &&
                HeuristicAgent::isLookaheadPoint(engine.state(), step.legal))
                out.emplace_back(engine.state(), step.legal);
            step = engine.applyChoice(std::max(0, scripted.selectIndex(engine.state(), step.legal)));
        }
        return out;
    }
};

// The turn's step budget stops the search after the simulation that spends
// it; later decisions that turn are answered as ScriptedAgent.
TEST_F(IsmctsBotTest, TurnBudgetCutsTheSearchThenFallsBackToScripted) {
    IsmctsConfig config;
    config.max_simulations = 100;
    config.max_steps_per_turn = 1;  // spent by the first simulation
    ScriptedAgent scripted(card_db);
    for (const auto& [state, legal] : lookaheadPoints(23, 4)) {
        IsmctsBot agent(card_db, card_registry, config);
        const Intent first = agent.selectAction(state, legal);
        EXPECT_NE(std::find(legal.begin(), legal.end(), first), legal.end());
        EXPECT_EQ(agent.stats().simulations, 1);
        EXPECT_EQ(agent.stats().budget_cut_searches, 1);
        EXPECT_GE(agent.stats().sim_steps, 1);
        // Same turn, budget spent.
        EXPECT_EQ(agent.selectAction(state, legal), scripted.selectAction(state, legal));
        EXPECT_EQ(agent.stats().budget_fallbacks, 1);
        EXPECT_EQ(agent.stats().simulations, 1);
    }
}

TEST_F(IsmctsBotTest, ReturnsALegalActionAndIsDeterministic) {
    IsmctsConfig config;
    config.max_simulations = 100;
    config.seed = 321;
    int largest_tree = 0;
    for (const auto& [state, legal] : lookaheadPoints(23, 8)) {
        IsmctsBot a(card_db, card_registry, config);
        IsmctsBot b(card_db, card_registry, config);
        const Intent pick = a.selectAction(state, legal);
        EXPECT_NE(std::find(legal.begin(), legal.end(), pick), legal.end());
        EXPECT_EQ(b.selectAction(state, legal), pick);
        EXPECT_EQ(a.stats().failed_simulations, 0);
        EXPECT_EQ(a.stats().simulations, config.max_simulations);
        largest_tree = std::max(largest_tree, a.stats().max_nodes);
    }
    // A decision whose every action ends the turn has no node below the
    // root (the tree stops at the horizon), so growth is required somewhere,
    // not everywhere.
    EXPECT_GT(largest_tree, 1) << "the tree never grew past the root";
}

TEST_F(IsmctsBotTest, NormalizedVisitPolicyCoversEveryLegalActionAndSumsToOne) {
    IsmctsConfig config;
    config.max_simulations = 200;
    config.final_policy_type = IsmctsFinalPolicyType::kNormalizedVisitCount;
    for (const auto& [state, legal] : lookaheadPoints(29, 5)) {
        IsmctsBot bot(card_db, card_registry, config);
        const auto policy = bot.RunSearch(state, legal);
        ASSERT_EQ(policy.size(), legal.size());
        for (const Intent& a : legal) {
            EXPECT_EQ(std::count_if(policy.begin(), policy.end(),
                                    [&](const auto& p) { return p.first == a; }),
                      1);
        }
        const double total = std::accumulate(policy.begin(), policy.end(), 0.0,
                                             [](double s, const auto& p) { return s + p.second; });
        EXPECT_NEAR(total, 1.0, 1e-9);
    }
}

TEST_F(IsmctsBotTest, BeatsRandom) {
    IsmctsConfig config;
    config.max_simulations = 100;
    int wins = 0;
    const int games = 10;
    for (int g = 0; g < games; ++g) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        config.seed = 1000 + g;
        IsmctsBot bot(card_db, card_registry, config);
        RandomAgent random(2000 + g);
        const bool bot_first = g % 2 == 0;
        const GameResult r = bot_first
            ? engine.runGame(deck1, deck2, bot, random, 3000 + g)
            : engine.runGame(deck1, deck2, random, bot, 3000 + g);
        if (r.winner == (bot_first ? PlayerId::Player1 : PlayerId::Player2)) ++wins;
        EXPECT_EQ(bot.stats().failed_simulations, 0)
            << "game " << g << ": key mismatches " << bot.stats().root_key_mismatches
            << ", legal mismatches " << bot.stats().root_legal_mismatches;
    }
    EXPECT_GE(wins, 8) << wins << "/" << games;
}

} // namespace
} // namespace riftbound
