/// Tests for the learned policy: the action encoding (ml/policy_features),
/// the ranking-tree reader (agents/policy_model), and its use by
/// HeuristicAgent together with the search observer that records its
/// training data.

#include "agents/heuristic_agent.h"
#include "agents/policy_model.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/policy_features.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

namespace riftbound {
namespace {

std::string rootPath(const std::string& rel) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/" + rel;
}

class PolicyModelTest : public ::testing::Test {
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

    struct Point { GameState state; std::vector<Intent> legal; };

    /// Decisions with 2+ legal actions from a random game.
    static std::vector<Point> decisions(uint64_t seed, size_t max_points) {
        const DeckSubmission d1 = DeckValidator::loadFromDeckList(rootPath("decks/leblanc_test.txt"), *db_);
        const DeckSubmission d2 = DeckValidator::loadFromDeckList(rootPath("decks/vex_test_deck.txt"), *db_);
        std::vector<Point> out;
        // Games go on (new seeds) until there are enough decisions.
        for (uint64_t g = 0; g < 8 && out.size() < max_points; ++g) {
            const uint64_t game_seed = seed + g * 1000;
            EventBus bus;
            GameEngine engine(*db_, bus, *registry_);
            std::mt19937_64 rng(game_seed);
            StepResult step = engine.beginGame(d1, d2, game_seed);
            for (int n = 0; n < 3000 && step.kind == StepKind::NeedDecision && out.size() < max_points; ++n) {
                if (step.legal.size() >= 2) out.push_back({engine.state(), step.legal});
                step = engine.applyChoice(static_cast<int>(
                    std::uniform_int_distribution<size_t>(0, step.legal.size() - 1)(rng)));
            }
        }
        return out;

    }

    /// A one-tree policy that scores ScriptedAgent's pick +1 and every other
    /// action -1, covering `kinds`: it must play as ScriptedAgent does.
    static std::string writeScriptedPolicy(const std::vector<std::string>& kinds) {
        const auto names = ml::policyFeatureNames(*db_);
        const int col = static_cast<int>(
            std::find(names.begin(), names.end(), "is_scripted_pick") - names.begin());
        std::string k;
        for (const auto& s : kinds) k += (k.empty() ? "\"" : ",\"") + s + "\"";
        const std::string path = testing::TempDir() + "scripted_policy.json";
        std::ofstream(path) << "{\"riftbound_policy_feature_version\": " << ml::kPolicyFeatureVersion
                            << ", \"kinds\": [" << k << "], \"lightgbm\": {\"objective\": \"lambdarank\", "
                            << "\"max_feature_idx\": " << col << ", \"tree_info\": [{\"tree_structure\": "
                            << "{\"split_feature\": " << col << ", \"threshold\": 0.5, "
                            << "\"decision_type\": \"<=\", \"default_left\": true, \"missing_type\": \"None\", "
                            << "\"left_child\": {\"leaf_value\": -1}, \"right_child\": {\"leaf_value\": 1}}}]}}";
        return path;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
};

CardRegistry* PolicyModelTest::registry_ = nullptr;
CardDB* PolicyModelTest::db_ = nullptr;

TEST_F(PolicyModelTest, EncodingIsSortedAndMarksTheAction) {
    const ScriptedAgent scripted(*db_);
    int checked = 0;
    for (const auto& [state, legal] : decisions(11, 300)) {
        ml::SparseRow position, row;
        ml::extractValueFeatures(state, legal[0].player, position);
        const char* kind = ml::policyDecisionKind(state, legal);
        const int pick = scripted.selectIndex(state, legal);
        for (size_t i = 0; i < legal.size(); ++i) {
            ml::extractPolicyFeatures(state, *db_, legal, static_cast<int>(i), position, kind, pick, row);
            ASSERT_GE(row.size(), position.size());
            EXPECT_TRUE(std::equal(position.begin(), position.end(), row.begin()));
            for (size_t j = 1; j < row.size(); ++j) ASSERT_LT(row[j - 1].first, row[j].first);
            EXPECT_LT(row.back().first, ml::kPolicyFeatureDim);
            const int type_col = ml::kPolicyActionOffset + static_cast<int>(legal[i].type);
            EXPECT_TRUE(std::any_of(row.begin(), row.end(),
                                    [&](const auto& kv) { return kv.first == type_col && kv.second == 1.0f; }));
            ++checked;
        }
    }
    EXPECT_GT(checked, 400);   // sample size of the 300 decisions; changes with the rules

    EXPECT_EQ(ml::policyFeatureNames(*db_).size(), static_cast<size_t>(ml::kPolicyFeatureDim));
}

TEST_F(PolicyModelTest, ScriptedPolicyPicksAsScriptedAndOnlyWhereItCovers) {
    const auto policy = PolicyModel::load(writeScriptedPolicy({"main", "chain", "rune", "choice"}));
    ASSERT_EQ(policy->numTrees(), 1);
    const ScriptedAgent scripted(*db_);
    int covered = 0, uncovered = 0;
    for (const auto& [state, legal] : decisions(12, 300)) {
        const int s = scripted.selectIndex(state, legal);
        const int p = policy->pick(state, *db_, legal, s);
        if (policy->covers(ml::policyDecisionKind(state, legal))) {
            EXPECT_EQ(p, s);
            ++covered;
        } else {
            EXPECT_EQ(p, -1);
            ++uncovered;
        }
    }
    EXPECT_GT(covered, 20);
    EXPECT_GT(uncovered, 0);
}

TEST_F(PolicyModelTest, RejectsAnotherFeatureVersion) {
    const std::string path = testing::TempDir() + "old_policy.json";
    std::ofstream(path) << "{\"riftbound_policy_feature_version\": 0, \"kinds\": [], "
                           "\"lightgbm\": {\"tree_info\": []}}";
    EXPECT_THROW(PolicyModel::load(path), std::runtime_error);
}

TEST_F(PolicyModelTest, AgentWithScriptedPolicyPlaysAsWithoutAndReportsItsSearches) {
    const auto policy = PolicyModel::load(writeScriptedPolicy(
        {"main", "showdown", "chain", "damage", "trigger", "trigger_cost", "rune", "choice"}));
    int searched = 0;
    for (const auto& [state, legal] : decisions(13, 400)) {
        if (!HeuristicAgent::isLookaheadPoint(state, legal)) continue;
        HeuristicConfig plain;
        plain.guesses = 2;
        HeuristicConfig with = plain;
        with.policy = policy;
        HeuristicAgent a(*db_, *registry_, plain), b(*db_, *registry_, with);
        std::vector<double> reported;
        b.setSearchObserver([&](const GameState&, const std::vector<Intent>& l,
                                const std::vector<double>& scores) {
            if (reported.empty() && l == legal) reported = scores;
        });
        const Intent pa = a.selectAction(state, legal);
        const Intent pb = b.selectAction(state, legal);
        EXPECT_TRUE(pa == pb) << pa.describe() << " vs " << pb.describe();
        ASSERT_EQ(reported.size(), legal.size());
        int best = -1;
        for (size_t i = 0; i < reported.size(); ++i)
            if (!std::isnan(reported[i]) && (best < 0 || reported[i] > reported[best]))
                best = static_cast<int>(i);
        ASSERT_GE(best, 0);
        EXPECT_TRUE(legal[best] == pb);
        if (++searched >= 6) break;
    }
    EXPECT_GE(searched, 3);
}

}  // namespace
}  // namespace riftbound
