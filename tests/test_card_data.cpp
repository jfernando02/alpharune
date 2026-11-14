/// @file test_card_data.cpp
/// Data-integrity tests on the loaded card database:
///   - Banned cards are actually marked CardDef::banned (official Standard ban list,
///     Rules Hub "Constructed Format Legality", last updated 2026-09-18).
///     (The --wild flag lets you PLAY banned cards; this verifies they're still
///     flagged on the card object.)
///   - Errata'd cards carry their corrected ability_text (from apply_errata.py
///     → registry, and post-refactor the C++ card literals).

#include <gtest/gtest.h>
#include "core/card_db.h"
#include "cards/card_registry.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace riftbound {
namespace {

class CardDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry.loadAll();
        db.buildFromClasses(registry);
    }
    CardRegistry registry;
    CardDB db;
};

// ── Banned cards ──────────────────────────────────────────────────────────
TEST_F(CardDataTest, BannedCardsAreMarkedBanned) {
    // Every name on the Standard ban list must be flagged on its CardDef.
    const char* banned[] = {
        "Called Shot", "Draven, Vanquisher", "Fight or Flight",
        "Scrapheap", "The Dreaming Tree", "Obelisk of Power",
        "Reaver's Row",
        // 2026-07-24
        "Stealthy Pursuer", "The Arena's Greatest", "Aspirant's Climb",
        // 2026-09-18
        "Ekko, Recurrent", "Stacked Deck",
    };
    for (const char* name : banned) {
        const CardDef* c = db.findByName(name);
        ASSERT_NE(c, nullptr) << name << " not found in registry";
        EXPECT_TRUE(c->banned) << name << " should be marked banned";
    }
}

// ── Printed keywords ──────────────────────────────────────────────────────
// A CardDef's keywords are the card's OWN keywords: the ones printed at the
// start of a line of its text ("[Tank]", "[Assault 2], [Shield 2]",
// "Ganking (...)"). A keyword the text only gives to something else ("give a
// unit [Ganking]", "play a token with [Temporary]", "[E]: [Reaction] - ...")
// must not be set: the engine reads these bits (Temporary kills the card,
// Hidden lets it be hidden, Deflect taxes choosing it, ...).
namespace {
const char* printedName(Keyword k) {
    return k == Keyword::QuickDraw ? "Quick-Draw" : toString(k);
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Keywords printed at the start of a line: a run of "[X]" / "[X N]" tokens
// separated by spaces or commas, or one bare keyword word.
std::set<std::string> leadingKeywords(const std::string& text) {
    std::set<std::string> out;
    std::istringstream in(text);
    std::string line;
    static const std::regex tok(R"(^\s*\[([^\]]*)\][\s,]*)");
    static const std::regex bare(R"(^\s*([A-Za-z-]+)(\s+\d+)?\s*(\(|,|\[|$))");   // "Equip [O] (...)"
    while (std::getline(in, line)) {
        std::smatch m;
        std::string rest = line;
        bool any = false;
        while (std::regex_search(rest, m, tok)) {
            std::string t = m[1];
            if (t == ">") { any = true; rest = m.suffix(); continue; }
            t = std::regex_replace(t, std::regex(R"(\s*\d+$)"), "");
            out.insert(lower(t));
            any = true;
            rest = m.suffix();
        }
        if (!any && std::regex_search(line, m, bare)) out.insert(lower(m[1]));
    }
    return out;
}
}  // namespace

TEST_F(CardDataTest, KeywordsAreOnlyThePrintedOnes) {
    // Forecaster ("Your Mechs have [Vision]") is a Mech: the play-time Vision
    // look reads printed keywords only, so its own Vision stays printed.
    const std::set<std::pair<CardDefId, Keyword>> allowed = {{388, Keyword::Vision}};
    for (const auto& [id, def] : db.all()) {
        const auto lead = leadingKeywords(def.ability_text);
        for (int i = 0; i < static_cast<int>(Keyword::Count); ++i) {
            const auto k = static_cast<Keyword>(i);
            if (!def.keywords.has(k) || allowed.count({id, k})) continue;
            // Quick-Draw: "(This has [Reaction]. ...)"
            if (k == Keyword::Reaction && def.keywords.has(Keyword::QuickDraw)) continue;
            EXPECT_TRUE(lead.count(lower(printedName(k))))
                << id << " " << def.name << " has [" << printedName(k)
                << "] but its text only mentions it: " << def.ability_text;
        }
    }
}

TEST_F(CardDataTest, PrintedKeywordsAreSet) {
    // A line that starts with "[Keyword]" followed by its reminder text (or
    // the end of the line) prints that keyword on the card. Not: ability
    // timing ("[Action][>] [E]: ...") or "[Empowered][>]" conditions.
    // Jayce, Hammer in Hand lists keywords as modes to choose from.
    const std::set<CardDefId> skip = {875};
    static const std::regex line_kw(R"(^\s*\[([A-Za-z-]+)(\s+\d+)?\](\s*\(|\s*$|,))");
    for (const auto& [id, def] : db.all()) {
        if (skip.count(id)) continue;
        std::istringstream in(def.ability_text);
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, line_kw)) continue;
            const std::string word = lower(m[1]);
            for (int i = 0; i < static_cast<int>(Keyword::Count); ++i) {
                const auto k = static_cast<Keyword>(i);
                if (lower(printedName(k)) == word)
                    EXPECT_TRUE(def.keywords.has(k))
                        << id << " " << def.name << " prints [" << printedName(k) << "]";
            }
        }
    }
}

TEST_F(CardDataTest, NonBannedCardIsNotBanned) {
    const CardDef* c = db.findByName("Blazing Scorcher");
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->banned) << "a legal card must not be flagged banned";
}

TEST_F(CardDataTest, BannedCountMatchesList) {
    int n = 0;
    for (const auto& [id, def] : db.all()) if (def.banned) ++n;
    // Exactly the 12 Standard ban-list names: a card banned by mistake is as
    // wrong as a missed ban. (Master Yi, Wuju Bladesman is 2v2-only; this
    // engine is 1v1, so he stays legal.)
    EXPECT_EQ(n, 12) << "banned-card count must match the Standard ban list";
}

// ── Errata adherence ──────────────────────────────────────────────────────
// Each card's ability_text must contain the distinctive phrase introduced by
// its official errata (apply_errata.py). If the data ever regresses to the
// pre-errata text, these fail.
TEST_F(CardDataTest, CardsAdhereToErrata) {
    struct Case { const char* name; const char* phrase; };
    const Case cases[] = {
        {"Disintegrate",       "If this kills it"},
        {"Convergent Mutation","increase its Might to the Might of another friendly unit"},
        {"Dune Drake",         "if there is a ready enemy unit here"},
        {"Dazzling Aurora",    "until you reveal a unit"},
        {"Blind Fury",         "Choose one and banish it"},
    };
    for (const auto& c : cases) {
        const CardDef* def = db.findByName(c.name);
        ASSERT_NE(def, nullptr) << c.name << " not found";
        EXPECT_NE(def->ability_text.find(c.phrase), std::string::npos)
            << c.name << " is missing errata phrase: \"" << c.phrase << "\"\n"
            << "  actual: " << def->ability_text;
    }
}

}  // namespace
}  // namespace riftbound
