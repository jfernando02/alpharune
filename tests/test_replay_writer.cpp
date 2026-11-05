/// @file test_replay_writer.cpp
/// The HTML replay's picture view: each snapshot carries the board as JSON
/// and the page lists the name and image of every card on it.

#include "cards/card_test_fixture.h"
#include "io/replay_writer.h"
#include "io/state_renderer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace riftbound::test {
namespace {

class ReplayWriterTest : public CardTestFixture {};

TEST_F(ReplayWriterTest, SnapshotCarriesTheBoardAndTheCardImages) {
    state.turn.phase = TurnPhase::MainPhase;
    const CardDefId unit_def = 1, hand_def = 2;
    auto unit = addUnit(P1, unit_def, 0, 0);
    state.getObject(unit).is_exhausted = true;
    state.getObject(unit).damage_marked = 1;
    addToHand(P2, hand_def);
    addRune(P1, Domain::Fury);

    Intent chosen;
    chosen.player = P1;
    chosen.card = unit;
    auto path = (std::filesystem::temp_directory_path() / "rb_replay_writer_test.html").string();
    {
        ReplayWriter writer(card_db, path);
        StateRenderer renderer(card_db);
        writer.recordDecision(state, {chosen, Intent{}}, chosen, renderer);
        writer.writeHtml();
    }
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string html = ss.str();
    std::filesystem::remove(path);

    const auto& def = card_db.get(unit_def);
    ASSERT_FALSE(def.image_url.empty());
    EXPECT_NE(html.find(std::to_string(unit_def) + ":{\"n\":\"" + def.name + "\""), std::string::npos);
    EXPECT_NE(html.find(def.image_url), std::string::npos);

    auto at = html.find("\n  b: ");
    ASSERT_NE(at, std::string::npos);
    at += 6;
    auto board = nlohmann::json::parse(html.substr(at, html.find('\n', at) - at - 1));
    const auto& on_bf = board["bfs"][0]["p1"];
    ASSERT_EQ(on_bf.size(), 1u);
    EXPECT_EQ(on_bf[0]["c"], unit_def);
    EXPECT_EQ(on_bf[0]["x"], 1);   // exhausted
    EXPECT_EQ(on_bf[0]["d"], 1);   // damage
    EXPECT_EQ(on_bf[0]["s"], 1);   // the decision's card
    EXPECT_EQ(board["players"][1]["hand"][0]["c"], hand_def);
    EXPECT_EQ(board["players"][0]["runes"].size(), 1u);
}

} // namespace
} // namespace riftbound::test
