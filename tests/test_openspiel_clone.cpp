/// Tests for RiftboundState's lazy clone: a clone that is advanced must play
/// the same game as the state it was cloned from, whichever way
/// `ensureLive` rebuilds its engine (resumeFromSnapshot or history replay).

#include "agents/scripted_agent.h"
#include "engine/game_engine.h"
#include "openspiel/action_vocab.h"
#include "openspiel/riftbound_game.h"
#include "openspiel/riftbound_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

namespace riftbound {
namespace {

std::string deckPath(const std::string& name) {
    const char* root = std::getenv("RIFTBOUND_ROOT");
    return std::string(root ? root : "..") + "/decks/" + name;
}

std::shared_ptr<const ::open_spiel::Game> makeGame(int seed) {
    ::open_spiel::GameParameters params;
    params["deck1"] = ::open_spiel::GameParameter(deckPath("leblanc_test.txt"));
    params["deck2"] = ::open_spiel::GameParameter(deckPath("vex_test_deck.txt"));
    params["seed"] = ::open_spiel::GameParameter(seed);
    return std::make_shared<openspiel::RiftboundGame>(params);
}

bool offersEndTurn(const std::vector<Intent>& legal) {
    return std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::EndTurn;
    });
}

/// A main-phase decision that looks like the open priority boundary (Open /
/// Neutral, empty chain, no cost payment) but is a choice asked inside an
/// action: the priority decision is the only one that offers EndTurn.
bool isMidActionChoice(const GameState& gs, const std::vector<Intent>& legal) {
    return gs.turn.phase == TurnPhase::MainPhase && gs.turn.isNeutralOpen() &&
           !gs.chain.exists() && !gs.cost_cursor.has_value() && !offersEndTurn(legal);
}

// At every decision of a scripted game, clone the state and apply the
// scripted action to both. The clone must reach the same decision as the
// original. Covers the decisions an action prompts for partway through
// (MakeChoice for an optional cost, a play location, ...), where the state
// looks like an open main-phase boundary but the engine is mid-action.
TEST(OpenSpielCloneTest, AdvancedCloneMatchesOriginalAtEveryDecision) {
    int mid_action_points = 0;
    int resumable_points = 0;
    for (int seed : {7, 8}) {
        auto game = makeGame(seed);
        const auto& rb_game = static_cast<const openspiel::RiftboundGame&>(*game);
        ScriptedAgent scripted(rb_game.cardDb());
        auto state = game->NewInitialState();
        auto& rb_state = static_cast<openspiel::RiftboundState&>(*state);

        for (int s = 0; s < 3000 && !state->IsTerminal(); ++s) {
            const GameState& gs = rb_state.engineState();
            const std::vector<Intent> legal = rb_state.legalIntents();
            const int pick = std::max(0, scripted.selectIndex(gs, legal));
            const auto action = static_cast<::open_spiel::Action>(
                openspiel::encodeAction(legal[pick], gs));
            if (isMidActionChoice(gs, legal)) {
                ++mid_action_points;
                EXPECT_FALSE(isCleanResumePoint(gs)) << "seed " << seed << ", decision " << s;
            }
            if (isCleanResumePoint(gs)) ++resumable_points;
            const int turn = gs.turn.turn_number;
            const auto phase = static_cast<int>(gs.turn.phase);

            auto clone = state->Clone();
            clone->ApplyAction(action);
            state->ApplyAction(action);

            const auto& clone_gs =
                static_cast<openspiel::RiftboundState&>(*clone).engineState();
            ASSERT_EQ(clone->IsTerminal(), state->IsTerminal())
                << "seed " << seed << ", decision " << s;
            ASSERT_EQ(clone->LegalActions(), state->LegalActions())
                << "seed " << seed << ", decision " << s << " (turn " << turn
                << ", phase " << phase << ") after " << legal[pick].describe();
            ASSERT_EQ(clone_gs.action_history, rb_state.engineState().action_history)
                << "seed " << seed << ", decision " << s;
        }
    }
    // Both ensureLive paths ran, and the scenario this test exists for occurred.
    EXPECT_GT(resumable_points, 0);
    EXPECT_GT(mid_action_points, 0);
}

} // namespace
} // namespace riftbound
