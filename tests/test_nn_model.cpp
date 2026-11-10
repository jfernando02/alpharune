/// Tests for the neural model (agents/nn_model): the file loader, the
/// hand-written forward pass against PyTorch's outputs for a committed
/// fixture, its use as a policy (PolicyModel, PolicyAgent) and as the tree
/// search's evaluator, and the search flags tree_skip_rune / tree_fpu.
///
/// tests/data/nn_fixture.* is written by riftbound-trainer's
/// `python -m riftbound_trainer.nn_fixture`: a few recorded positions, a tiny
/// seeded network and PyTorch's outputs for them. Rerun it when the
/// observation or the network changes.

#include "agents/heuristic_agent.h"
#include "agents/nn_model.h"
#include "agents/policy_agent.h"
#include "agents/policy_model.h"
#include "agents/random_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/obs_io.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace riftbound {
namespace {

std::string rootPath(const std::string& rel) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/" + rel;
}

const std::string kFixture = "tests/data/nn_fixture";

class NnModelTest : public ::testing::Test {
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
    }

    void SetUp() override {
        deck1 = DeckValidator::loadFromDeckList(rootPath("decks/leblanc_test.txt"), *db_);
        deck2 = DeckValidator::loadFromDeckList(rootPath("decks/vex_test_deck.txt"), *db_);
    }

    static std::string writeBytes(const std::string& name, const std::string& bytes) {
        const std::string path = testing::TempDir() + name;
        std::ofstream(path, std::ios::binary) << bytes;
        return path;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
    DeckSubmission deck1, deck2;
};

CardRegistry* NnModelTest::registry_ = nullptr;
CardDB* NnModelTest::db_ = nullptr;

TEST_F(NnModelTest, RejectsBadFiles) {
    EXPECT_THROW(NnModel::load(testing::TempDir() + "no_such_model.rbnn"), std::runtime_error);
    EXPECT_THROW(NnModel::load(writeBytes("bad_magic.rbnn", "RBNN9999\x10\0\0\0{}")), std::runtime_error);

    std::ifstream in(rootPath(kFixture + ".rbnn"), std::ios::binary);
    const std::string good((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_GT(good.size(), 2000u);
    EXPECT_FALSE(NnModel::isNnFile(rootPath("decks/leblanc_test.txt")));
    EXPECT_TRUE(NnModel::isNnFile(rootPath(kFixture + ".rbnn")));
    // Cut short: the header promises tensors the data does not hold.
    EXPECT_THROW(NnModel::load(writeBytes("cut.rbnn", good.substr(0, good.size() - 1000))), std::runtime_error);
}

// The engine's forward pass reproduces PyTorch's numbers for the fixture.
TEST_F(NnModelTest, FixtureMatchesPyTorch) {
    const auto model = NnModel::load(rootPath(kFixture + ".rbnn"));
    ASSERT_TRUE(model->hasValue());
    ASSERT_TRUE(model->hasPolicy());
    EXPECT_TRUE(model->covers("main"));
    EXPECT_FALSE(model->covers("chain"));

    const auto positions = ml::readShard(rootPath(kFixture));
    std::ifstream expected(rootPath(kFixture + ".expected.txt"));
    ASSERT_TRUE(expected.good());
    std::string line;
    size_t p = 0;
    int compared = 0;
    for (; std::getline(expected, line) && !line.empty(); ++p) {
        ASSERT_LT(p, positions.size());
        std::istringstream fields(line);
        size_t index = 0;
        double want_value = 0.0;
        fields >> index >> want_value;
        std::vector<double> want_logits;
        for (double x; fields >> x;) want_logits.push_back(x);
        ASSERT_EQ(index, p);

        double value = 0.0;
        std::vector<double> logits;
        model->forward(positions[p].obs, &positions[p].actions, &value, &logits);
        const auto close = [](double got, double want) {
            return std::abs(got - want) <= 1e-4 * std::max(1.0, std::abs(want));
        };
        EXPECT_TRUE(close(value, want_value)) << "position " << p << ": " << value << " vs " << want_value;
        ASSERT_EQ(logits.size(), want_logits.size()) << "position " << p;
        for (size_t a = 0; a < logits.size(); ++a, ++compared)
            EXPECT_TRUE(close(logits[a], want_logits[a]))
                << "position " << p << " action " << a << ": " << logits[a] << " vs " << want_logits[a];
    }
    EXPECT_EQ(p, positions.size());
    EXPECT_GT(compared, 10);
}

// Loaded as a policy and a value model, it scores real decisions: only the
// kinds it covers, one finite score per legal action, a probability.
TEST_F(NnModelTest, ServesAsPolicyAndValueModel) {
    const auto policy = PolicyModel::load(rootPath(kFixture + ".rbnn"));
    const auto value = loadPositionModel(rootPath(kFixture + ".rbnn"));
    EventBus bus;
    GameEngine engine(*db_, bus, *registry_);
    ScriptedAgent scripted(*db_);
    StepResult step = engine.beginGame(deck1, deck2, 31);
    int scored = 0;
    for (int n = 0; n < 1500 && step.kind == StepKind::NeedDecision; ++n) {
        const auto& legal = step.legal;
        const int s = std::max(0, scripted.selectIndex(engine.state(), legal));
        if (legal.size() >= 2) {
            const char* kind = ml::policyDecisionKind(engine.state(), legal);
            const int pick = policy->pick(engine.state(), *db_, legal, s);
            if (policy->covers(kind)) {
                ASSERT_GE(pick, 0);
                ASSERT_LT(pick, static_cast<int>(legal.size()));
                const auto scores = policy->scores(engine.state(), *db_, legal, kind, s);
                ASSERT_EQ(scores.size(), legal.size());
                for (double x : scores) EXPECT_TRUE(std::isfinite(x));
                ++scored;
            } else {
                EXPECT_EQ(pick, -1);
            }
            const double w = value->winProbability(engine.state(), legal[0].player, *db_);
            EXPECT_GT(w, 0.0);
            EXPECT_LT(w, 1.0);
        }
        step = engine.applyChoice(s);
    }
    EXPECT_GT(scored, 10);
}

TEST_F(NnModelTest, PolicyAgentPlaysWholeGames) {
    const auto policy = PolicyModel::load(rootPath(kFixture + ".rbnn"));
    for (int g = 0; g < 2; ++g) {
        EventBus bus;
        GameEngine engine(*db_, bus, *registry_);
        PolicyAgentConfig config;
        config.policy = policy;
        config.temperature = g == 0 ? 0.0 : 0.3;
        config.epsilon = g == 0 ? 0.0 : 0.05;
        config.seed = 500 + g;
        PolicyAgent agent(*db_, config);
        ScriptedAgent scripted(*db_);
        GameResult r;
        ASSERT_NO_THROW(r = engine.runGame(deck1, deck2, agent, scripted, 600 + g));
        EXPECT_NE(r.winner, PlayerId::None) << "game " << g;
    }
}

// The tree search with the network as its value model and policy, and with
// the rune-payment and first-play-urgency flags on, plays whole games.
TEST_F(NnModelTest, TreeSearchWithNetworkAndFlagsPlaysWholeGames) {
    HeuristicConfig config;
    config.tree = true;
    config.tree_sims = 16;
    config.tree_skip_rune = true;
    config.tree_fpu = 0.1;
    config.model = loadPositionModel(rootPath(kFixture + ".rbnn"));
    config.policy = PolicyModel::load(rootPath(kFixture + ".rbnn"));
    EventBus bus;
    GameEngine engine(*db_, bus, *registry_);
    config.seed = 77;
    HeuristicAgent tree(*db_, *registry_, config);
    RandomAgent random(78);
    GameResult r;
    ASSERT_NO_THROW(r = engine.runGame(deck1, deck2, tree, random, 79));
    EXPECT_NE(r.winner, PlayerId::None);
    EXPECT_GT(tree.stats().tree_searches, 0);
    EXPECT_EQ(tree.stats().failed_simulations, 0);
}

// tree_skip_rune: no in-tree node for a rune payment, so the stat that counts
// them stays 0 where the default search counts some.
TEST_F(NnModelTest, SkipRuneLeavesRunePaymentsOutOfTheTree) {
    long with_flag = 0, without = 0, searches = 0;
    for (bool skip : {false, true}) {
        HeuristicConfig config;
        config.tree = true;
        config.tree_sims = 24;
        config.tree_skip_rune = skip;
        config.seed = 91;
        EventBus bus;
        GameEngine engine(*db_, bus, *registry_);
        HeuristicAgent tree(*db_, *registry_, config);
        ScriptedAgent scripted(*db_);
        GameResult r;
        ASSERT_NO_THROW(r = engine.runGame(deck1, deck2, tree, scripted, 92));
        EXPECT_EQ(tree.stats().failed_simulations, 0);
        (skip ? with_flag : without) = tree.stats().tree_rune_steps;
        searches += tree.stats().tree_searches;
    }
    EXPECT_GT(searches, 0);
    EXPECT_EQ(with_flag, 0);
    EXPECT_GT(without, 0);
}

}  // namespace
}  // namespace riftbound
