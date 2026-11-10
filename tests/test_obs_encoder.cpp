/// Tests for the neural model's observation (ml/obs_encoder): what one
/// player sees, the entity order, gear hosts, battlefield slots, and the
/// action references into the entities.

#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/obs_encoder.h"
#include "ml/policy_features.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <variant>

namespace riftbound {
namespace {

std::string rootPath(const std::string& rel) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/" + rel;
}

class ObsEncoderTest : public ::testing::Test {
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
        EventBus bus;
        GameEngine engine(*db_, bus, *registry_);
        std::mt19937_64 rng(seed);
        StepResult step = engine.beginGame(d1, d2, seed);
        for (int n = 0; n < 3000 && step.kind == StepKind::NeedDecision && out.size() < max_points; ++n) {
            if (step.legal.size() >= 2) out.push_back({engine.state(), step.legal});
            step = engine.applyChoice(static_cast<int>(
                std::uniform_int_distribution<size_t>(0, step.legal.size() - 1)(rng)));
        }
        return out;
    }

    static bool sameEncoding(const ml::Obs& a, const ml::Obs& b) {
        if (std::memcmp(a.g, b.g, sizeof a.g) != 0) return false;
        if (a.entities.size() != b.entities.size() || a.bags != b.bags) return false;
        for (size_t i = 0; i < a.entities.size(); ++i) {
            const ml::ObsEntity &x = a.entities[i], &y = b.entities[i];
            if (x.card != y.card || x.group != y.group || x.place != y.place || x.pool != y.pool ||
                x.host != y.host || x.object != y.object || std::memcmp(x.f, y.f, sizeof x.f) != 0)
                return false;
        }
        return true;
    }

    static CardRegistry* registry_;
    static CardDB* db_;
};

CardRegistry* ObsEncoderTest::registry_ = nullptr;
CardDB* ObsEncoderTest::db_ = nullptr;

constexpr uint8_t group(ml::ObsGroup g) { return static_cast<uint8_t>(g); }

TEST_F(ObsEncoderTest, SeesOwnHandNotTheOpponentsAndIsDeterministic) {
    int checked = 0, gear_seen = 0;
    auto points = decisions(21, 250);
    for (auto& more : decisions(23, 250)) points.push_back(std::move(more));
    for (const auto& [state, legal] : points) {
        const PlayerId me = legal[0].player;
        ml::Obs obs, again;
        ml::encodeObs(state, me, *db_, obs);

        // Own hand: exactly the hand's objects, as MyHand.
        std::set<GameObjectId> hand_seen;
        for (const auto& e : obs.entities)
            if (e.group == group(ml::ObsGroup::MyHand)) hand_seen.insert(e.object);
        const auto& hand = state.player(me).hand;
        EXPECT_EQ(hand_seen, std::set<GameObjectId>(hand.begin(), hand.end()));
        // The opponent's hand is only a count.
        for (GameObjectId id : state.player(opponent(me)).hand) EXPECT_EQ(obs.find(id), -1);
        EXPECT_EQ(obs.g[ml::glob::OppHand], static_cast<float>(state.player(opponent(me)).hand.size()));

        // A copy encodes the same, entity for entity.
        const GameState copy = state;
        ml::encodeObs(copy, me, *db_, again);
        EXPECT_TRUE(sameEncoding(obs, again));

        // Gear hosts point at units; battlefields match the state's.
        int battlefields = 0;
        for (size_t i = 0; i < obs.entities.size(); ++i) {
            const auto& e = obs.entities[i];
            if (e.host >= 0) {
                ASSERT_LT(static_cast<size_t>(e.host), obs.entities.size());
                const uint8_t g = obs.entities[e.host].group;
                EXPECT_TRUE(g == group(ml::ObsGroup::MyUnit) || g == group(ml::ObsGroup::OppUnit));
                EXPECT_TRUE(e.group == group(ml::ObsGroup::MyGear) || e.group == group(ml::ObsGroup::OppGear));
                ++gear_seen;
            }
            if (e.group == group(ml::ObsGroup::Battlefield)) {
                ASSERT_LT(battlefields, static_cast<int>(state.battlefields.size()));
                EXPECT_EQ(e.place, 3 + battlefields);
                EXPECT_EQ(e.object, state.battlefields[battlefields].card_object_id);
                ++battlefields;
            }
        }
        EXPECT_EQ(battlefields, static_cast<int>(state.battlefields.size()));
        ++checked;
    }
    EXPECT_GT(checked, 100);
    (void)gear_seen;  // decks without equipment never attach any
}

TEST_F(ObsEncoderTest, ActionsReferToTheirEntities) {
    int acting = 0, chosen = 0, moved = 0, dest = 0;
    for (const auto& [state, legal] : decisions(22, 300)) {
        ml::Obs obs;
        ml::encodeObs(state, legal[0].player, *db_, obs);
        std::vector<ml::ObsAction> actions;
        ml::encodeActions(state, *db_, legal, obs, ml::policyDecisionKind(state, legal), -1, actions);
        ASSERT_EQ(actions.size(), legal.size());
        for (size_t i = 0; i < legal.size(); ++i) {
            const Intent& a = legal[i];
            const ml::ObsAction& o = actions[i];
            const GameObjectId src = a.card != kInvalidId ? a.card : a.ability_source;
            EXPECT_EQ(o.acting, obs.find(src));
            if (o.acting >= 0) {
                EXPECT_EQ(obs.entities[o.acting].object, src);
                ++acting;
            }
            for (size_t t = 0; t < a.targets.size() && t < static_cast<size_t>(ml::kObsMaxChosen); ++t) {
                EXPECT_EQ(o.chosen[t], obs.find(a.targets[t]));
                ++chosen;
            }
            ASSERT_LE(o.n_moved, ml::kObsMaxMoved);
            for (int m = 0; m < o.n_moved; ++m) {
                EXPECT_EQ(o.moved[m], obs.find(a.units_to_move[m]));
                ++moved;
            }
            const auto* to = a.move_destination ? std::get_if<BattlefieldLocation>(&*a.move_destination) : nullptr;
            if (to) {
                ASSERT_GE(o.dest, 0);
                ASSERT_LT(static_cast<size_t>(o.dest), state.battlefields.size());
                EXPECT_EQ(state.battlefields[o.dest].id, to->id);
                ++dest;
            }
        }
    }
    EXPECT_GT(acting, 50);
    EXPECT_GT(moved + dest, 0);
    (void)chosen;
}

}  // namespace
}  // namespace riftbound
