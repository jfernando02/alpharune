/// @file decision_census.cpp
/// Which decisions does an agent actually face? Plays games between two
/// copies of an agent and counts every decision with 2+ legal actions by
/// kind: the main-phase priority decision the look-ahead agents search, and
/// the ones they hand to ScriptedAgent (chain responses, showdown focus,
/// combat damage, targets / choices, optional triggers, mulligans, ...).
///
///   riftbound_decision_census [--agent SPEC] [--games N] [--seed S] [--decks DIR]

#include "tool_common.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>

using namespace riftbound;

namespace {

/// tools::decisionKind with a description (offsearch=random:<kind> takes the kind).
std::string kindOf(const GameState& s, const std::vector<Intent>& legal) {
    static const std::map<std::string, std::string> describe = {
        {"main", "main priority (searched)"},
        {"main_mid", "main priority, mid-action (not resumable)"},
        {"mulligan", "mulligan"},
        {"damage", "combat damage assignment"},
        {"showdown", "showdown focus: play / pass (searched with showdowns=1)"},
        {"chain", "chain response: react / pass (searched with chain=1)"},
        {"chain_mid", "chain response: react / pass (not resumable)"},
        {"trigger", "optional trigger"},
        {"trigger_cost", "triggered cost"},
        {"battlefield", "choose battlefield"},
        {"rune", "choice: rune payment"},
        {"resolve", "choice: during resolution (target / mode / X)"},
        {"choice", "choice: other (play location, optional cost, ...)"},
    };
    const std::string kind = tools::decisionKind(s, legal);
    const auto it = describe.find(kind);
    return kind + "  " + (it != describe.end() ? it->second : "");
}

struct Counter { long n = 0; long legal = 0; };

/// Counts the decisions of the wrapped agent.
class CensusAgent : public AgentInterface {
public:
    CensusAgent(AgentInterface& inner, std::map<std::string, Counter>& counts)
        : inner_(inner), counts_(counts) {}
    Intent selectAction(const GameState& s, const std::vector<Intent>& legal) override {
        if (legal.size() >= 2) {
            auto& c = counts_[kindOf(s, legal)];
            ++c.n;
            c.legal += static_cast<long>(legal.size());
        }
        return inner_.selectAction(s, legal);
    }

private:
    AgentInterface& inner_;
    std::map<std::string, Counter>& counts_;
};

} // namespace

int main(int argc, char** argv) {
    std::string spec = "heuristic", decks_dir = "decks";
    int games = 40;
    uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--agent") spec = v;
        else if (k == "--games") games = std::stoi(v);
        else if (k == "--seed") seed = std::stoull(v);
        else if (k == "--decks") decks_dir = v;
    }
    CardRegistry registry;
    registry.loadAll();
    CardDB db;
    db.buildFromClasses(registry);
    const auto agent_spec = tools::parseToolAgent(spec);
    const auto deck_set = tools::loadLegalDecks(decks_dir, db);

    std::map<std::string, Counter> counts;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, deck_set.decks.size() - 1);
    for (int g = 0; g < games; ++g) {
        auto a = tools::makeToolAgent(agent_spec, db, registry, rng());
        auto b = tools::makeToolAgent(agent_spec, db, registry, rng());
        CensusAgent ca(*a, counts), cb(*b, counts);
        EventBus bus;
        GameEngine engine(db, bus, registry);
        try {
            engine.runGame(deck_set.decks[pick(rng)], deck_set.decks[pick(rng)], ca, cb,
                           (rng() & 0x7FFFFFFFULL) | 1);
        } catch (const std::exception& e) {
            std::cerr << "game " << g << " threw: " << e.what() << "\n";
        }
    }

    long total = 0;
    for (const auto& [k, c] : counts) total += c.n;
    std::vector<std::pair<std::string, Counter>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second.n > y.second.n; });
    std::cout << "decisions with 2+ legal actions over " << games << " games: " << total << "\n";
    for (const auto& [k, c] : rows)
        std::cout << std::setw(7) << c.n << "  " << std::fixed << std::setprecision(1)
                  << std::setw(5) << 100.0 * c.n / total << "%  avg legal "
                  << std::setw(5) << static_cast<double>(c.legal) / c.n << "  " << k << "\n";
    return 0;
}
