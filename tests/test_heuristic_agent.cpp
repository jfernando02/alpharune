/// Tests for the look-ahead agent and its parts: the shared resume check,
/// the position evaluator, determinization and HeuristicAgent itself.
///
/// Positions come from real games driven through the step-machine API with
/// ScriptedAgent, so every state is one the engine actually produces.

#include "agents/determinize.h"
#include "agents/evaluator.h"
#include "agents/heuristic_agent.h"
#include "agents/random_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "core/game_object.h"
#include "engine/game_engine.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

class HeuristicAgentTest : public ::testing::Test {
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

    /// Up to `count` look-ahead points from one scripted-vs-scripted game,
    /// each with the engine's legal actions there. Only decisions with two
    /// or more options are kept.
    std::vector<std::pair<GameState, std::vector<Intent>>>
    cleanPoints(uint64_t seed, int count) {
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

// ─── Resume ──────────────────────────────────────────────────────────────────

TEST_F(HeuristicAgentTest, ResumedCopyOffersTheSameLegalActions) {
    std::vector<std::pair<GameState, std::vector<Intent>>> points;
    for (uint64_t seed : {7, 8, 9, 10, 11}) {
        auto more = cleanPoints(seed, 40);
        points.insert(points.end(), more.begin(), more.end());
    }
    ASSERT_GE(points.size(), 50u);
    for (const auto& [state, legal] : points) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        const StepResult step = engine.resumeFromSnapshot(state, 99);
        ASSERT_EQ(step.kind, StepKind::NeedDecision);
        EXPECT_EQ(step.legal, legal)
            << "turn " << state.turn.turn_number << ", phase "
            << static_cast<int>(state.turn.phase) << ": expected "
            << legal[0].describe() << " (+" << legal.size() - 1 << "), got "
            << (step.legal.empty() ? std::string("nothing") : step.legal[0].describe());
    }
}

// ─── Evaluator ───────────────────────────────────────────────────────────────

TEST_F(HeuristicAgentTest, EvaluateIsAntisymmetric) {
    for (const auto& [state, legal] : cleanPoints(11, 10))
        EXPECT_DOUBLE_EQ(evaluate(state, PlayerId::Player1),
                         -evaluate(state, PlayerId::Player2));
}

TEST_F(HeuristicAgentTest, EvaluateRewardsPointsAndWins) {
    const auto points = cleanPoints(11, 1);
    ASSERT_FALSE(points.empty());
    GameState state = points[0].first;
    const double before = evaluate(state, PlayerId::Player1);

    state.player(PlayerId::Player1).score += 1;
    EXPECT_DOUBLE_EQ(evaluate(state, PlayerId::Player1), before + EvalWeights{}.points);

    state.game_over = true;
    state.winner = PlayerId::Player1;
    EXPECT_EQ(evaluate(state, PlayerId::Player1), kWinScore);
    EXPECT_EQ(evaluate(state, PlayerId::Player2), -kWinScore);
}

TEST_F(HeuristicAgentTest, EvaluateCountsATrashReplayableCardLikeAHandCard) {
    const auto points = cleanPoints(11, 1);
    ASSERT_FALSE(points.empty());
    GameState state = points[0].first;
    PlayerState& me = state.player(PlayerId::Player1);
    ASSERT_FALSE(me.hand.empty());
    const double in_hand = evaluate(state, PlayerId::Player1);

    // Move a hand card to the trash: one card fewer in hand.
    const GameObjectId card = me.hand.back();
    me.hand.pop_back();
    me.trash.push_back(card);
    state.getObject(card).zone = ZoneType::Trash;
    const double in_trash = evaluate(state, PlayerId::Player1);
    EXPECT_DOUBLE_EQ(in_trash, in_hand - EvalWeights{}.hand);

    // Granting a replay from the trash restores its value, once per card.
    PlayerState::TrashReplayGrant grant;
    grant.card = card;
    me.trash_replay_grants.push_back(grant);
    me.trash_replay_grants.push_back(grant);
    EXPECT_DOUBLE_EQ(evaluate(state, PlayerId::Player1), in_hand);
}

TEST_F(HeuristicAgentTest, EvaluateIgnoresHiddenCardIdentities) {
    std::mt19937_64 rng(3);
    for (const auto& [state, legal] : cleanPoints(13, 10)) {
        GameState guess = state;
        determinize(guess, PlayerId::Player1, rng);
        EXPECT_DOUBLE_EQ(evaluate(guess, PlayerId::Player1),
                         evaluate(state, PlayerId::Player1));
    }
}

TEST_F(HeuristicAgentTest, RecyclingARuneCostsMoreEarlyThanLate) {
    const auto points = cleanPoints(11, 1);
    ASSERT_FALSE(points.empty());
    EvalWeights w{};
    w.points = w.controlled_bf = w.contested_might = w.board_might = 0;
    w.hand = w.trash_replayable = w.runes = 0;
    w.rune_energy = 1.0;
    const PlayerId me = PlayerId::Player1;

    // Move one of `me`'s runes from base to the rune deck, as a power payment does.
    const auto recycleOne = [&](GameState s) {
        const GameObjectId rune = s.runesInBase(me).front();
        auto& obj = s.getObject(rune);
        obj.location.reset();
        obj.zone = ZoneType::RuneDeck;
        s.player(me).rune_deck.insert(s.player(me).rune_deck.begin(), rune);
        return s;
    };
    // Every rune `me` owns in base: the late game.
    const auto channelAll = [&](GameState s) {
        for (GameObjectId id : s.player(me).rune_deck) {
            auto& obj = s.getObject(id);
            obj.location = BaseLocation{me};
            obj.zone = ZoneType::Base;
        }
        s.player(me).rune_deck.clear();
        return s;
    };

    const GameState early = points[0].first;
    ASSERT_FALSE(early.runesInBase(me).empty());
    ASSERT_GE(early.player(me).rune_deck.size(), 4u);
    const double early_cost = evaluate(early, me, w) - evaluate(recycleOne(early), me, w);

    const GameState late = channelAll(early);
    const double late_cost = evaluate(late, me, w) - evaluate(recycleOne(late), me, w);

    EXPECT_GE(early_cost, 4.0) << "a rune short for several turns";
    EXPECT_EQ(late_cost, 0.0) << "back at the next channel";
}

// ─── Rune payment and mulligan ───────────────────────────────────────────────

TEST_F(HeuristicAgentTest, EnergyIsPaidWithoutADecision) {
    int power_picks = 0;
    for (uint64_t seed : {7, 8, 9}) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        ScriptedAgent scripted(card_db);
        StepResult step = engine.beginGame(deck1, deck2, seed);
        for (int s = 0; s < 3000 && step.kind == StepKind::NeedDecision; ++s) {
            const auto& cursor = engine.state().cost_cursor;
            if (cursor) {
                EXPECT_EQ(cursor->phase, GameState::CostPaymentCursor::Phase::PayingPower);
                ++power_picks;
                // Exhausted runes are offered first: recycling one costs no Energy.
                const auto& first = engine.state().getObject(step.legal[0].chosen_objects[0]);
                for (const Intent& a : step.legal)
                    if (engine.state().getObject(a.chosen_objects[0]).is_exhausted)
                        EXPECT_TRUE(first.is_exhausted);
            }
            step = engine.applyChoice(std::max(0, scripted.selectIndex(engine.state(), step.legal)));
        }
    }
    EXPECT_GT(power_picks, 0) << "no power payment seen; the test checks nothing";
}

TEST_F(HeuristicAgentTest, ScriptedMulliganKeepsEarlyDropsElseSetsAsideTwo) {
    ScriptedAgent scripted(card_db);
    int kept = 0, mulliganed = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        StepResult step = engine.beginGame(deck1, deck2, seed);
        // Battlefield choices come first, then one mulligan per player.
        for (int d = 0; d < 6 && step.kind == StepKind::NeedDecision; ++d) {
            if (step.legal[0].type != IntentType::MulliganDecision) {
                step = engine.applyChoice(std::max(0, scripted.selectIndex(engine.state(), step.legal)));
                continue;
            }
            const GameState& s = engine.state();
            const PlayerId me = step.legal[0].player;
            const int max_drop = me == s.turn.starting_player ? 2 : 3;
            const auto& hand = s.player(me).hand;
            const auto energy = [&](GameObjectId id) {
                return card_db.get(s.getObject(id).card_def_id).energy_cost;
            };
            const auto unit_drop = [&](GameObjectId id) {
                return s.getObject(id).card_type == CardType::Unit &&
                       energy(id) >= 2 && energy(id) <= max_drop;
            };
            // Both test decks hold units at these costs, so only units count.
            const auto& deck = s.player(me).main_deck;
            ASSERT_TRUE(std::any_of(hand.begin(), hand.end(), unit_drop) ||
                        std::any_of(deck.begin(), deck.end(), unit_drop));
            const bool has_drop = std::any_of(hand.begin(), hand.end(), unit_drop);

            const int pick = scripted.selectIndex(s, step.legal);
            ASSERT_GE(pick, 0);
            const auto& set_aside = step.legal[pick].cards_to_mulligan;
            if (has_drop) {
                EXPECT_TRUE(set_aside.empty());
                ++kept;
            } else {
                ASSERT_EQ(set_aside.size(), 2u);
                // Nothing kept costs more Energy than what was set aside.
                int cheapest_aside = std::min(energy(set_aside[0]), energy(set_aside[1]));
                for (GameObjectId id : hand)
                    if (std::find(set_aside.begin(), set_aside.end(), id) == set_aside.end())
                        EXPECT_LE(energy(id), cheapest_aside);
                ++mulliganed;
            }
            step = engine.applyChoice(pick);
        }
    }
    EXPECT_GT(kept, 0);
    EXPECT_GT(mulliganed, 0);
}

TEST_F(HeuristicAgentTest, ScriptedMulliganCountsSpellDropsOnlyWithoutUnitDrops) {
    ScriptedAgent scripted(card_db);
    int checked = 0;
    for (uint64_t seed = 1; seed <= 40 && checked < 5; ++seed) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        StepResult step = engine.beginGame(deck1, deck2, seed);
        while (step.kind == StepKind::NeedDecision &&
               step.legal[0].type != IntentType::MulliganDecision)
            step = engine.applyChoice(std::max(0, scripted.selectIndex(engine.state(), step.legal)));
        ASSERT_EQ(step.kind, StepKind::NeedDecision);

        GameState s = engine.state();
        const PlayerId me = step.legal[0].player;
        const int max_drop = me == s.turn.starting_player ? 2 : 3;
        const auto at_cost = [&](GameObjectId id) {
            const int e = card_db.get(s.getObject(id).card_def_id).energy_cost;
            return e >= 2 && e <= max_drop;
        };
        auto& hand = s.player(me).hand;
        if (std::none_of(hand.begin(), hand.end(), at_cost)) continue;

        // Make every drop-cost card in the deck list a spell: no unit drops left.
        for (auto* pile : {&hand, &s.player(me).main_deck})
            for (GameObjectId id : *pile)
                if (at_cost(id)) s.getObject(id).card_type = CardType::Spell;

        const int pick = scripted.selectIndex(s, step.legal);
        ASSERT_GE(pick, 0);
        EXPECT_TRUE(step.legal[pick].cards_to_mulligan.empty())
            << "a spell at drop cost is the drop when the deck has no unit drops";
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

// ─── Determinize ─────────────────────────────────────────────────────────────

TEST_F(HeuristicAgentTest, DeterminizeKeepsPilesAndPublicObjects) {
    const PlayerId me = PlayerId::Player1;
    const PlayerId them = PlayerId::Player2;
    std::mt19937_64 rng(5);
    for (const auto& [state, legal] : cleanPoints(17, 10)) {
        GameState guess = state;
        determinize(guess, me, rng);

        const auto& was = state.player(them);
        const auto& now = guess.player(them);
        EXPECT_EQ(now.hand.size(), was.hand.size());
        EXPECT_EQ(now.main_deck.size(), was.main_deck.size());
        EXPECT_EQ(guess.player(me).hand, state.player(me).hand);

        // Same cards overall, each in the zone its pile says.
        std::multiset<GameObjectId> before(was.hand.begin(), was.hand.end());
        before.insert(was.main_deck.begin(), was.main_deck.end());
        std::multiset<GameObjectId> after(now.hand.begin(), now.hand.end());
        after.insert(now.main_deck.begin(), now.main_deck.end());
        EXPECT_EQ(after, before);
        for (GameObjectId id : now.hand)
            EXPECT_EQ(guess.getObject(id).zone, ZoneType::Hand);
        for (GameObjectId id : now.main_deck)
            EXPECT_EQ(guess.getObject(id).zone, ZoneType::MainDeck);

        // Nothing outside the hidden piles moves.
        for (const auto& [id, obj] : state.objects) {
            if (before.count(id)) continue;
            EXPECT_EQ(guess.getObject(id).zone, obj.zone) << "object " << id;
        }
    }
}

TEST_F(HeuristicAgentTest, DeterminizeIsSeededAndCoversThePool) {
    const auto points = cleanPoints(19, 1);
    ASSERT_FALSE(points.empty());
    const GameState& state = points[0].first;
    const auto& theirs = state.player(PlayerId::Player2);
    ASSERT_FALSE(theirs.hand.empty());

    GameState a = state, b = state;
    std::mt19937_64 rng_a(42), rng_b(42);
    determinize(a, PlayerId::Player1, rng_a);
    determinize(b, PlayerId::Player1, rng_b);
    EXPECT_EQ(a.player(PlayerId::Player2).hand, b.player(PlayerId::Player2).hand);
    EXPECT_EQ(a.player(PlayerId::Player1).main_deck, b.player(PlayerId::Player1).main_deck);

    std::set<GameObjectId> pool(theirs.hand.begin(), theirs.hand.end());
    pool.insert(theirs.main_deck.begin(), theirs.main_deck.end());
    std::set<GameObjectId> seen_in_hand;
    std::mt19937_64 rng(1);
    for (int i = 0; i < 500; ++i) {
        GameState guess = state;
        determinize(guess, PlayerId::Player1, rng);
        const auto& hand = guess.player(PlayerId::Player2).hand;
        seen_in_hand.insert(hand.begin(), hand.end());
    }
    EXPECT_EQ(seen_in_hand, pool);
}

// ─── HeuristicAgent ──────────────────────────────────────────────────────────

TEST_F(HeuristicAgentTest, ReturnsALegalActionAndIsDeterministic) {
    HeuristicConfig config;
    config.guesses = 2;
    config.seed = 123;
    for (const auto& [state, legal] : cleanPoints(23, 8)) {
        HeuristicAgent a(card_db, card_registry, config);
        HeuristicAgent b(card_db, card_registry, config);
        const Intent pick = a.selectAction(state, legal);
        EXPECT_NE(std::find(legal.begin(), legal.end(), pick), legal.end());
        EXPECT_EQ(b.selectAction(state, legal), pick);
        EXPECT_EQ(a.stats().failed_simulations, 0);
        EXPECT_EQ(a.stats().lookahead_decisions, 1);
    }
}

TEST_F(HeuristicAgentTest, BeatsRandom) {
    HeuristicConfig config;
    config.guesses = 2;
    int wins = 0;
    const int games = 10;
    for (int g = 0; g < games; ++g) {
        EventBus bus;
        GameEngine engine(card_db, bus, card_registry);
        config.seed = 1000 + g;
        HeuristicAgent heuristic(card_db, card_registry, config);
        RandomAgent random(2000 + g);
        // Alternate seats so neither deck nor turn order decides the result.
        const bool heuristic_first = g % 2 == 0;
        const GameResult r = heuristic_first
            ? engine.runGame(deck1, deck2, heuristic, random, 3000 + g)
            : engine.runGame(deck1, deck2, random, heuristic, 3000 + g);
        const PlayerId seat = heuristic_first ? PlayerId::Player1 : PlayerId::Player2;
        if (r.winner == seat) ++wins;
        EXPECT_EQ(heuristic.stats().failed_simulations, 0) << "game " << g;
    }
    EXPECT_GE(wins, 9) << wins << "/" << games;
}

} // namespace
} // namespace riftbound
