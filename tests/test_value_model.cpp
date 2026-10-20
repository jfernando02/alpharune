/// Tests for the learned value model: the position encoding
/// (ml/value_features) and the gradient-boosted-tree reader
/// (agents/gbt_evaluator).

#include "agents/determinize.h"
#include "agents/gbt_evaluator.h"
#include "agents/heuristic_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "core/game_object.h"
#include "engine/game_engine.h"
#include "ml/value_features.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace riftbound {
namespace {

std::string rootPath(const std::string& rel) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/" + rel;
}

class ValueModelTest : public ::testing::Test {
protected:
    CardRegistry card_registry;
    CardDB card_db;
    DeckSubmission deck1, deck2;

    void SetUp() override {
        card_registry.loadAll();
        card_db.buildFromClasses(card_registry);
        deck1 = DeckValidator::loadFromDeckList(rootPath("decks/leblanc_test.txt"), card_db);
        deck2 = DeckValidator::loadFromDeckList(rootPath("decks/vex_test_deck.txt"), card_db);
    }

    /// Positions at the first decision of each turn of a scripted game.
    std::vector<GameState> turnStarts(uint64_t seed) {
        std::vector<GameState> out;
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        ScriptedAgent scripted(card_db);
        StepResult step = engine.beginGame(deck1, deck2, seed);
        int last_turn = -1;
        for (int s = 0; s < 3000 && step.kind == StepKind::NeedDecision; ++s) {
            const GameState& st = engine.state();
            if (st.turn.phase != TurnPhase::Setup && st.turn.phase != TurnPhase::Mulligan &&
                st.turn.turn_number != last_turn) {
                last_turn = st.turn.turn_number;
                out.push_back(st);
            }
            step = engine.applyChoice(std::max(0, scripted.selectIndex(st, step.legal)));
        }
        return out;
    }

    static float value(const ml::SparseRow& row, int index) {
        for (const auto& [i, v] : row)
            if (i == index) return v;
        return 0.0f;
    }
};

TEST_F(ValueModelTest, EncodingIsSortedAndInRange) {
    for (const GameState& s : turnStarts(5)) {
        ml::SparseRow row;
        ml::extractValueFeatures(s, PlayerId::Player1, row);
        ASSERT_FALSE(row.empty());
        for (size_t k = 0; k < row.size(); ++k) {
            EXPECT_GE(row[k].first, 0);
            EXPECT_LT(row[k].first, ml::kValueFeatureDim);
            EXPECT_NE(row[k].second, 0.0f);
            if (k > 0) EXPECT_LT(row[k - 1].first, row[k].first);
        }
    }
}

TEST_F(ValueModelTest, EncodingHidesTheOpponentsHand) {
    std::mt19937_64 rng(3);
    for (const GameState& s : turnStarts(7)) {
        ml::SparseRow before, after;
        ml::extractValueFeatures(s, PlayerId::Player1, before);
        GameState guess = s;
        determinize(guess, PlayerId::Player1, rng);
        ml::extractValueFeatures(guess, PlayerId::Player1, after);
        EXPECT_EQ(before, after) << "turn " << s.turn.turn_number;
    }
}

TEST_F(ValueModelTest, PerspectivesMirrorEachOther) {
    const auto names = ml::valueFeatureNames(card_db);
    const auto index = [&](const std::string& name) {
        return static_cast<int>(std::find(names.begin(), names.end(), name) - names.begin());
    };
    for (const GameState& s : turnStarts(9)) {
        ml::SparseRow p1, p2;
        ml::extractValueFeatures(s, PlayerId::Player1, p1);
        ml::extractValueFeatures(s, PlayerId::Player2, p2);
        for (const char* pair : {"points", "bf_held", "board_might", "units", "hand_size"}) {
            EXPECT_EQ(value(p1, index(std::string("my_") + pair)),
                      value(p2, index(std::string("opp_") + pair))) << pair;
        }
        EXPECT_EQ(value(p1, index("hand_eval")), -value(p2, index("hand_eval")));
        EXPECT_NE(value(p1, index("is_my_turn")), value(p2, index("is_my_turn")));
        // Each side's card blocks swap with the other's.
        for (int c = 0; c < ml::kCardVocabSize; ++c) {
            EXPECT_EQ(value(p1, ml::valueBlockOffset(ml::ValueBlock::MyBase) + c),
                      value(p2, ml::valueBlockOffset(ml::ValueBlock::OppBase) + c));
            EXPECT_EQ(value(p1, ml::valueBlockOffset(ml::ValueBlock::MyLegend) + c),
                      value(p2, ml::valueBlockOffset(ml::ValueBlock::OppLegend) + c));
        }
    }
}

TEST_F(ValueModelTest, BoardCardsLandInTheirBlock) {
    int units_seen = 0;
    for (const GameState& s : turnStarts(11)) {
        ml::SparseRow row;
        ml::extractValueFeatures(s, PlayerId::Player1, row);
        for (const auto& [id, obj] : s.objects) {
            if (!obj.isUnit() || !obj.location || obj.card_def_id == kInvalidId) continue;
            if (obj.zone != ZoneType::Base && obj.zone != ZoneType::BattlefieldZone) continue;
            const bool mine = obj.controller == PlayerId::Player1;
            const bool at_bf = obj.zone == ZoneType::BattlefieldZone;
            const auto block = mine ? (at_bf ? ml::ValueBlock::MyBattlefields : ml::ValueBlock::MyBase)
                                    : (at_bf ? ml::ValueBlock::OppBattlefields : ml::ValueBlock::OppBase);
            EXPECT_GE(value(row, ml::valueBlockOffset(block) + obj.card_def_id - 1), 1.0f)
                << obj.name;
            ++units_seen;
        }
    }
    EXPECT_GT(units_seen, 0);
}

// The engine's tree reader against LightGBM's own raw scores, on a small
// model and rows that riftbound-trainer's value_gbt.py --fixture-dir wrote.
TEST_F(ValueModelTest, TreeReaderMatchesLightGbm) {
    const auto model = GbtModel::load(rootPath("tests/data/value_gbt_fixture.json"));
    ASSERT_GT(model->numTrees(), 0);
    std::ifstream rows(rootPath("tests/data/value_gbt_fixture_rows.txt"));
    ASSERT_TRUE(rows) << "fixture rows missing";
    int checked = 0;
    for (std::string line; std::getline(rows, line);) {
        std::istringstream in(line);
        double expected;
        in >> expected;
        ml::SparseRow row;
        for (std::string item; in >> item;) {
            const auto colon = item.find(':');
            row.emplace_back(std::stoi(item.substr(0, colon)), std::stof(item.substr(colon + 1)));
        }
        EXPECT_NEAR(model->rawScore(row), expected, 1e-9) << "row " << checked;
        ++checked;
    }
    EXPECT_GT(checked, 100);
}

TEST_F(ValueModelTest, RejectsAModelForAnotherFeatureVersion) {
    const std::string path = ::testing::TempDir() + "stale_model.json";
    std::ofstream(path) << R"({"riftbound_value_feature_version": 0, "lightgbm": {}})";
    EXPECT_THROW(GbtModel::load(path), std::runtime_error);
}

} // namespace
} // namespace riftbound
