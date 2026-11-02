/// @file arena.cpp
/// Head-to-head match between two agents over the legal decks of a directory.
///
///   riftbound_arena --a SPEC --b SPEC [--games N] [--threads T] [--seed S]
///                   [--decks DIR]
///
/// SPEC is random | scripted | heuristic[:model=FILE,guesses=N,turns_ahead=N,step_budget=N,
/// <weight>=X] (see tools/tool_common.h). Games are played in pairs on the
/// same random deck pair and game seed with the seats swapped, so neither
/// agent gains from going first or from a lucky draw of decks; N is rounded
/// up to an even number.
///
/// Progress goes to stderr; the last line of stdout is the result as JSON:
///   {"games": N, "a_wins": .., "b_wins": .., "draws": .., "a_score": ..}
/// where a_score = (a_wins + draws / 2) / games, the number riftbound-trainer's
/// selfplay loop promotes a candidate on, followed by "pairs", "a_pairs",
/// "b_pairs" and "split_pairs": pairs A or B won more of, and pairs split 1-1
/// (where the decks or the seat decided both games, saying nothing about the
/// agents).

#include "tool_common.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace riftbound;

namespace {

struct Options {
    std::string a, b;
    std::string decks_dir = "decks";
    int games = 200;
    int threads = 0;
    uint64_t seed = 1;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--a") o.a = next();
        else if (arg == "--b") o.b = next();
        else if (arg == "--decks") o.decks_dir = next();
        else if (arg == "--games") o.games = std::stoi(next());
        else if (arg == "--threads") o.threads = std::stoi(next());
        else if (arg == "--seed") o.seed = std::stoull(next());
        else throw std::runtime_error("unknown argument " + arg);
    }
    if (o.a.empty() || o.b.empty()) throw std::runtime_error("--a and --b are required");
    if (o.games < 1) throw std::runtime_error("--games must be positive");
    if (o.threads <= 0) o.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    tools::ToolAgentSpec spec_a, spec_b;
    try {
        opt = parse(argc, argv);
        spec_a = tools::parseToolAgent(opt.a);
        spec_b = tools::parseToolAgent(opt.b);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"
                  << "usage: riftbound_arena --a SPEC --b SPEC [--games N] [--threads T] "
                     "[--seed S] [--decks DIR]\n"
                  << "  SPEC: random | scripted | heuristic[:model=FILE,guesses=N,step_budget=N,"
                     "turns_ahead=N,<weight>=X]\n";
        return 2;
    }

    CardRegistry registry;
    registry.loadAll();
    CardDB db;
    db.buildFromClasses(registry);

    const tools::DeckSet deck_set = tools::loadLegalDecks(opt.decks_dir, db);
    const auto& decks = deck_set.decks;
    if (decks.empty()) {
        std::cerr << "no legal decks in " << opt.decks_dir << "\n";
        return 1;
    }

    const int pairs = (opt.games + 1) / 2;
    std::atomic<int> next_pair{0}, games_done{0}, a_wins{0}, b_wins{0}, draws{0}, errors{0};
    // Per pair: A won more of its two games, B did, or they split. A pair
    // splits whenever the decks or the seat decide both games, so only the
    // other pairs say which agent is better.
    std::atomic<int> a_pairs{0}, b_pairs{0}, split_pairs{0};
    std::mutex log_mutex;
    const auto start = std::chrono::steady_clock::now();

    const auto worker = [&] {
        for (int p = next_pair++; p < pairs; p = next_pair++) {
            std::mt19937_64 rng(opt.seed * 1000003ULL + static_cast<uint64_t>(p));
            std::uniform_int_distribution<size_t> pick_deck(0, decks.size() - 1);
            const size_t d1 = pick_deck(rng), d2 = pick_deck(rng);
            const uint64_t game_seed = (rng() & 0x7FFFFFFFULL) | 1;
            const uint64_t seed_a = rng(), seed_b = rng();

            // Game 0: A in seat 1. Game 1: the same game with the seats swapped.
            double a_points = 0.0;  // A's wins in this pair, a draw counting half
            for (int swap = 0; swap < 2; ++swap) {
                auto agent_a = tools::makeToolAgent(spec_a, db, registry, seed_a);
                auto agent_b = tools::makeToolAgent(spec_b, db, registry, seed_b);
                AgentInterface& p1 = swap ? *agent_b : *agent_a;
                AgentInterface& p2 = swap ? *agent_a : *agent_b;
                const PlayerId a_seat = swap ? PlayerId::Player2 : PlayerId::Player1;

                EventBus bus;
                GameEngine engine(db, bus, registry);
                try {
                    const GameResult r = engine.runGame(decks[d1], decks[d2], p1, p2, game_seed);
                    if (r.winner == a_seat) { ++a_wins; a_points += 1.0; }
                    else if (r.winner == PlayerId::None) { ++draws; a_points += 0.5; }
                    else ++b_wins;
                } catch (const std::exception& e) {
                    // Scored as a draw so a crash favours neither agent.
                    ++errors;
                    ++draws;
                    a_points += 0.5;
                    std::lock_guard<std::mutex> lock(log_mutex);
                    std::cerr << "pair " << p << " game " << swap << " threw: " << e.what() << "\n";
                }

                const int done = ++games_done;
                if (done % 50 == 0) {
                    const double secs = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start).count();
                    std::lock_guard<std::mutex> lock(log_mutex);
                    std::cerr << done << "/" << pairs * 2 << " games: A " << a_wins.load()
                              << " B " << b_wins.load() << " draws " << draws.load() << ", "
                              << secs << "s\n";
                }
            }
            if (a_points > 1.0) ++a_pairs;
            else if (a_points < 1.0) ++b_pairs;
            else ++split_pairs;
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < opt.threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    const int games = pairs * 2;
    const double score = (a_wins.load() + 0.5 * draws.load()) / games;
    if (errors.load() > 0) std::cerr << errors.load() << " games threw (scored as draws)\n";
    std::cout << "{\"games\": " << games << ", \"a_wins\": " << a_wins.load()
              << ", \"b_wins\": " << b_wins.load() << ", \"draws\": " << draws.load()
              << ", \"errors\": " << errors.load() << ", \"a_score\": " << score
              << ", \"pairs\": " << pairs << ", \"a_pairs\": " << a_pairs.load()
              << ", \"b_pairs\": " << b_pairs.load() << ", \"split_pairs\": " << split_pairs.load()
              << "}\n";
    return 0;
}
