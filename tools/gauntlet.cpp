/// @file gauntlet.cpp
/// How well do some decks do against a field? Plays each candidate deck
/// against every legal deck of a directory, with the same agent in both seats.
///
///   riftbound_gauntlet --agent SPEC --deck FILE [--deck FILE ...]
///                      [--opponents DIR] [--pairs N] [--threads T] [--seed S]
///                      [--game-seconds N] [--out FILE.csv]
///
/// Each candidate meets each opponent in N pairs of games: the same game seed
/// with the seats swapped, so neither side gains from going first. Pair p
/// against an opponent uses the same seeds for every candidate, so candidates
/// that differ by a few cards are compared on the same games as far as the
/// decks let them. Opponents are the legal decks of DIR that are not
/// candidates. A game still going after --game-seconds (default 600) is
/// finished by ScriptedAgent for both seats.
///
/// Prints a table per candidate (overall and per opponent) and writes every
/// game to --out: candidate,opponent,pair,candidate_seat,result (win / loss /
/// draw / error),turns,time_limited.

#include "tool_common.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using namespace riftbound;

namespace {

struct Options {
    std::string agent = "heuristic";
    std::vector<std::string> decks;
    std::string opponents_dir = "decks";
    int pairs = 1;
    int threads = 0;
    uint64_t seed = 1;
    double game_seconds = 600.0;
    std::string out;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--agent") o.agent = next();
        else if (arg == "--deck") o.decks.push_back(next());
        else if (arg == "--opponents") o.opponents_dir = next();
        else if (arg == "--pairs") o.pairs = std::stoi(next());
        else if (arg == "--threads") o.threads = std::stoi(next());
        else if (arg == "--seed") o.seed = std::stoull(next());
        else if (arg == "--game-seconds") o.game_seconds = std::stod(next());
        else if (arg == "--out") o.out = next();
        else throw std::runtime_error("unknown argument " + arg);
    }
    if (o.decks.empty()) throw std::runtime_error("at least one --deck is required");
    if (o.pairs < 1) throw std::runtime_error("--pairs must be positive");
    if (o.threads <= 0) o.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

/// Past the game's time limit, both seats play as ScriptedAgent.
class ClockedAgent : public AgentInterface {
public:
    ClockedAgent(AgentInterface& inner, std::chrono::steady_clock::time_point start,
                 double limit, bool& tripped, const CardDB& db)
        : inner_(inner), start_(start), limit_(limit), tripped_(tripped), scripted_(db) {}
    Intent selectAction(const GameState& state, const std::vector<Intent>& legal) override {
        if (!tripped_ && limit_ > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() > limit_)
            tripped_ = true;
        return tripped_ ? scripted_.selectAction(state, legal) : inner_.selectAction(state, legal);
    }

private:
    AgentInterface& inner_;
    std::chrono::steady_clock::time_point start_;
    double limit_;
    bool& tripped_;
    ScriptedAgent scripted_;
};

struct Tally {
    int wins = 0, losses = 0, draws = 0, errors = 0;
    int games() const { return wins + losses + draws + errors; }
    double score() const {
        return games() ? (wins + 0.5 * (draws + errors)) / games() : 0.0;
    }
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    tools::ToolAgentSpec spec;
    try {
        opt = parse(argc, argv);
        spec = tools::parseToolAgent(opt.agent);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"
                  << "usage: riftbound_gauntlet --agent SPEC --deck FILE [--deck FILE ...] "
                     "[--opponents DIR] [--pairs N] [--threads T] [--seed S] "
                     "[--game-seconds N] [--out FILE.csv]\n";
        return 2;
    }

    CardRegistry registry;
    registry.loadAll();
    CardDB db;
    db.buildFromClasses(registry);
    DeckValidator validator(db);

    namespace fs = std::filesystem;
    std::vector<std::string> names;
    std::vector<DeckSubmission> candidates;
    for (const auto& path : opt.decks) {
        DeckSubmission d = DeckValidator::loadFromDeckList(path, db);
        const auto v = validator.validate(d);
        if (!v.is_legal) {
            std::cerr << path << " is not a legal deck:\n";
            for (const auto& err : v.errors) std::cerr << "  " << err << "\n";
            return 1;
        }
        names.push_back(fs::path(path).stem().string());
        candidates.push_back(std::move(d));
    }

    const tools::DeckSet field = tools::loadLegalDecks(opt.opponents_dir, db);
    std::vector<size_t> opponents;
    for (size_t i = 0; i < field.names.size(); ++i)
        if (std::find(names.begin(), names.end(), field.names[i]) == names.end())
            opponents.push_back(i);
    if (opponents.empty()) {
        std::cerr << "no opponent decks in " << opt.opponents_dir << "\n";
        return 1;
    }

    // One job: a candidate against an opponent, one pair of games.
    const size_t jobs = candidates.size() * opponents.size() * static_cast<size_t>(opt.pairs);
    std::atomic<size_t> next_job{0};
    std::atomic<int> games_done{0};
    std::mutex mutex;
    std::map<std::pair<size_t, size_t>, Tally> tallies;  // (candidate, opponent)
    std::vector<std::string> rows;
    const auto start = std::chrono::steady_clock::now();

    const auto worker = [&] {
        for (size_t j = next_job++; j < jobs; j = next_job++) {
            // Candidates vary fastest, so each (opponent, pair) is played by
            // every candidate close together in time.
            const size_t c = j % candidates.size();
            const size_t rest = j / candidates.size();
            const size_t o = opponents[rest % opponents.size()];
            const int p = static_cast<int>(rest / opponents.size());
            std::mt19937_64 rng(opt.seed * 1000003ULL + o * 7919ULL + static_cast<uint64_t>(p));
            const uint64_t game_seed = (rng() & 0x7FFFFFFFULL) | 1;
            const uint64_t seed_c = rng(), seed_o = rng();

            for (int swap = 0; swap < 2; ++swap) {
                auto agent_c = tools::makeToolAgent(spec, db, registry, seed_c);
                auto agent_o = tools::makeToolAgent(spec, db, registry, seed_o);
                bool tripped = false;
                const auto t0 = std::chrono::steady_clock::now();
                ClockedAgent cc(*agent_c, t0, opt.game_seconds, tripped, db);
                ClockedAgent co(*agent_o, t0, opt.game_seconds, tripped, db);
                const PlayerId c_seat = swap ? PlayerId::Player2 : PlayerId::Player1;

                std::string result;
                int turns = 0;
                EventBus bus;
                GameEngine engine(db, bus, registry);
                try {
                    const GameResult r = swap
                        ? engine.runGame(field.decks[o], candidates[c], co, cc, game_seed)
                        : engine.runGame(candidates[c], field.decks[o], cc, co, game_seed);
                    turns = r.total_turns;
                    result = r.winner == c_seat ? "win"
                           : r.winner == PlayerId::None ? "draw" : "loss";
                } catch (const std::exception& e) {
                    result = "error";
                    std::lock_guard<std::mutex> lock(mutex);
                    std::cerr << names[c] << " vs " << field.names[o] << " pair " << p
                              << " game " << swap << " threw: " << e.what() << "\n";
                }

                std::lock_guard<std::mutex> lock(mutex);
                Tally& t = tallies[{c, o}];
                if (result == "win") ++t.wins;
                else if (result == "loss") ++t.losses;
                else if (result == "draw") ++t.draws;
                else ++t.errors;
                rows.push_back(names[c] + "," + field.names[o] + "," + std::to_string(p) + "," +
                               (swap ? "2" : "1") + "," + result + "," + std::to_string(turns) +
                               "," + (tripped ? "1" : "0"));
                const int done = ++games_done;
                if (done % 20 == 0) {
                    const double secs = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start).count();
                    std::cerr << done << "/" << jobs * 2 << " games, " << secs << "s\n";
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < opt.threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    if (!opt.out.empty()) {
        std::ofstream out(opt.out);
        out << "candidate,opponent,pair,candidate_seat,result,turns,time_limited\n";
        for (const auto& r : rows) out << r << "\n";
    }

    std::cout << std::fixed << std::setprecision(3);
    for (size_t c = 0; c < candidates.size(); ++c) {
        Tally all;
        for (size_t o : opponents) {
            const Tally& t = tallies[{c, o}];
            all.wins += t.wins;
            all.losses += t.losses;
            all.draws += t.draws;
            all.errors += t.errors;
        }
        std::cout << names[c] << ": " << all.wins << "-" << all.losses << "-" << all.draws
                  << " (errors " << all.errors << ") score " << all.score() << "\n";
        for (size_t o : opponents) {
            const Tally& t = tallies[{c, o}];
            std::cout << "    " << std::setw(24) << std::left << field.names[o] << std::right
                      << " " << t.wins << "-" << t.losses << "-" << t.draws << "\n";
        }
    }
    return 0;
}
