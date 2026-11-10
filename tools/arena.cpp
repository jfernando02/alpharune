/// @file arena.cpp
/// Head-to-head match between two agents over the legal decks of a directory.
///
///   riftbound_arena --a SPEC --b SPEC [--games N] [--threads T] [--seed S]
///                   [--decks DIR] [--mirror] [--game-seconds N]
///                   [--sprt S0,S1] [--sprt-alpha X] [--sprt-beta X] [--sprt-min-pairs N]
///
/// SPEC is any agent tools/tool_common.h parses (random, scripted,
/// heuristic[:...], ismcts[:...]). Games are played in pairs on the same random
/// deck pair and game seed with the seats swapped, so neither agent gains from
/// going first or from a lucky draw of decks; N is rounded up to an even
/// number. --mirror gives both seats the same deck, so fewer pairs are decided
/// by one deck being stronger and more of them by the agents.
///
/// A game still going after --game-seconds (default 300; 0 = no limit) is
/// finished by ScriptedAgent for both seats, so neither gains from it. A
/// board flooded with tokens (Eye of the Herald Recruits) makes every engine
/// step slow, and a look-ahead agent can then spend hours on one game even
/// within its per-turn step budget.
///
/// --sprt S0,S1 makes the match a sequential test of "A scores S1" against
/// "A scores S0" (e.g. 0.5,0.53): it stops as soon as the pairs played so far
/// decide between them, instead of always playing --games, which is then the
/// most it will play. A clear result takes a fraction of the games; a close
/// one keeps going. The statistic is the generalized sequential probability
/// ratio on per-pair scores x in {0, 1/4, 1/2, 3/4, 1} (as chess engine
/// testing does for game pairs):
///
///     LLR = n (S1 - S0) (2 mean(x) - S0 - S1) / (2 var(x))
///
/// over the first n pairs in pair order (so slow games cannot bias when it
/// stops), stopping at LLR >= log((1 - beta) / alpha) (A is better: "H1") or
/// LLR <= log(beta / (1 - alpha)) ("H0"), after at least --sprt-min-pairs.
/// alpha and beta (default 0.05 each) bound the chance of each wrong answer.
/// The JSON then has "sprt": {s0, s1, llr, lower, upper, pairs_tested,
/// decision: "H1" | "H0" | "none"}.
///
/// Progress goes to stderr; the last line of stdout is the result as JSON:
///   games, a_wins, b_wins, draws, errors, a_score = (a_wins + draws/2) / games,
///   (games and pairs are those played, fewer than asked when --sprt stopped early)
///   pairs, a_pairs, b_pairs, split_pairs: pairs A or B won more of, and pairs
///     split 1-1 (decided by the decks or the seat, saying nothing about the
///     agents) — riftbound-trainer's sign test runs on a_pairs vs b_pairs;
///   a_think_s, b_think_s: seconds each agent spent choosing, summed over
///     games (so it grows with --threads' worth of parallel games);
///   a_searched, b_searched, a_fallbacks, b_fallbacks: a look-ahead agent's
///     searched decisions and those answered without search because its
///     turn's budget was spent;
///   time_limited: games finished by ScriptedAgent after --game-seconds.
/// Before it, stderr gets each agent's search counts (tools::SearchCounts).

#include "tool_common.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
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
    bool mirror = false;
    double game_seconds = 300.0;
    bool sprt = false;
    double sprt_s0 = 0.5, sprt_s1 = 0.53, sprt_alpha = 0.05, sprt_beta = 0.05;
    int sprt_min_pairs = 40;
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
        else if (arg == "--mirror") o.mirror = true;
        else if (arg == "--game-seconds") o.game_seconds = std::stod(next());
        else if (arg == "--sprt") {
            const std::string v = next();
            const auto comma = v.find(',');
            if (comma == std::string::npos) throw std::runtime_error("--sprt takes S0,S1");
            o.sprt = true;
            o.sprt_s0 = std::stod(v.substr(0, comma));
            o.sprt_s1 = std::stod(v.substr(comma + 1));
            if (!(o.sprt_s0 < o.sprt_s1)) throw std::runtime_error("--sprt needs S0 < S1");
        }
        else if (arg == "--sprt-alpha") o.sprt_alpha = std::stod(next());
        else if (arg == "--sprt-beta") o.sprt_beta = std::stod(next());
        else if (arg == "--sprt-min-pairs") o.sprt_min_pairs = std::stoi(next());
        else throw std::runtime_error("unknown argument " + arg);
    }
    if (o.a.empty() || o.b.empty()) throw std::runtime_error("--a and --b are required");
    if (o.games < 1) throw std::runtime_error("--games must be positive");
    if (o.threads <= 0) o.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

/// One game's wall clock, shared by both seats: past the limit both play as
/// ScriptedAgent.
struct GameClock {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double limit = 0.0;  // seconds; 0 = none
    bool tripped = false;
    bool over() {
        if (!tripped && limit > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > limit)
            tripped = true;
        return tripped;
    }
};

/// Adds the time the wrapped agent spends choosing to `seconds`, and hands its
/// decisions to ScriptedAgent once the game is over its time limit.
class TimedAgent : public AgentInterface {
public:
    TimedAgent(AgentInterface& inner, double& seconds, GameClock& clock, const CardDB& db)
        : inner_(inner), seconds_(seconds), clock_(clock), scripted_(db) {}
    Intent selectAction(const GameState& state, const std::vector<Intent>& legal) override {
        if (clock_.over()) return scripted_.selectAction(state, legal);
        const auto t = std::chrono::steady_clock::now();
        Intent out = inner_.selectAction(state, legal);
        seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        return out;
    }

private:
    AgentInterface& inner_;
    double& seconds_;
    GameClock& clock_;
    ScriptedAgent scripted_;
};

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
                     "[--seed S] [--decks DIR] [--mirror] [--game-seconds N]\n"
                     "                       [--sprt S0,S1] [--sprt-alpha X] [--sprt-beta X] "
                     "[--sprt-min-pairs N]\n"
                  << "  SPEC: random | scripted\n"
                     "      | heuristic[:model=FILE,guesses=N,turns_ahead=N,step_budget=N,showdowns=0/1,\n"
                     "                   followups=N,chain=0/1,setup=0/1,battlefield=0/1,mulligan=0/1,\n"
                     "                   setup_guesses=N,setup_turns=N,policy=FILE,\n"
                     "                   policy_rollouts=0/1,policy_offsearch=0/1,\n"
                     "                   <weight>=X]\n"
                     "      | ismcts[:sims=N,uct_c=X,turns_ahead=N,step_budget=N,model=FILE,<weight>=X]\n";
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
    std::mutex mutex;  // stderr, and the totals below
    double a_think = 0.0, b_think = 0.0;
    long a_searched = 0, b_searched = 0, a_fallbacks = 0, b_fallbacks = 0;
    tools::SearchCounts a_counts, b_counts;
    int time_limited = 0;
    const auto start = std::chrono::steady_clock::now();

    // Sequential test: each finished pair's score, and the test over the
    // unbroken run of finished pairs from pair 0.
    std::atomic<bool> stop{false};
    std::vector<double> pair_score(pairs, -1.0);  // under `mutex`
    int sprt_tested = 0;
    double sprt_llr = 0.0;
    std::string sprt_decision = "none";
    const double sprt_lower = std::log(opt.sprt_beta / (1.0 - opt.sprt_alpha));
    const double sprt_upper = std::log((1.0 - opt.sprt_beta) / opt.sprt_alpha);
    const auto sprtUpdate = [&](int pair, double score) {  // call with `mutex` held
        pair_score[pair] = score;
        if (!opt.sprt || stop.load()) return;
        int n = sprt_tested;
        while (n < pairs && pair_score[n] >= 0.0) ++n;
        if (n == sprt_tested) return;
        sprt_tested = n;
        double sum = 0.0, squares = 0.0;
        for (int i = 0; i < n; ++i) {
            sum += pair_score[i];
            squares += pair_score[i] * pair_score[i];
        }
        const double mean = sum / n;
        const double var = std::max(squares / n - mean * mean, 1e-6);
        sprt_llr = n * (opt.sprt_s1 - opt.sprt_s0) * (2.0 * mean - opt.sprt_s0 - opt.sprt_s1) / (2.0 * var);
        if (n < opt.sprt_min_pairs) return;
        if (sprt_llr >= sprt_upper) sprt_decision = "H1";
        else if (sprt_llr <= sprt_lower) sprt_decision = "H0";
        else return;
        stop = true;
        std::cerr << "sprt: " << sprt_decision << " after " << n << " pairs (LLR " << sprt_llr << ")\n";
    };

    const auto worker = [&] {
        for (int p = next_pair++; p < pairs && !stop.load(); p = next_pair++) {
            std::mt19937_64 rng(opt.seed * 1000003ULL + static_cast<uint64_t>(p));
            std::uniform_int_distribution<size_t> pick_deck(0, decks.size() - 1);
            const size_t d1 = pick_deck(rng), d2_drawn = pick_deck(rng);
            const size_t d2 = opt.mirror ? d1 : d2_drawn;
            const uint64_t game_seed = (rng() & 0x7FFFFFFFULL) | 1;
            const uint64_t seed_a = rng(), seed_b = rng();

            // Game 0: A in seat 1. Game 1: the same game with the seats swapped.
            double a_points = 0.0;  // A's wins in this pair, a draw counting half
            for (int swap = 0; swap < 2; ++swap) {
                auto agent_a = tools::makeToolAgent(spec_a, db, registry, seed_a);
                auto agent_b = tools::makeToolAgent(spec_b, db, registry, seed_b);
                double think_a = 0.0, think_b = 0.0;
                GameClock clock;
                clock.limit = opt.game_seconds;
                TimedAgent timed_a(*agent_a, think_a, clock, db), timed_b(*agent_b, think_b, clock, db);
                AgentInterface& p1 = swap ? static_cast<AgentInterface&>(timed_b) : timed_a;
                AgentInterface& p2 = swap ? static_cast<AgentInterface&>(timed_a) : timed_b;
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
                    std::lock_guard<std::mutex> lock(mutex);
                    std::cerr << "pair " << p << " game " << swap << " threw: " << e.what() << "\n";
                }

                const auto ca = tools::searchCounts(*agent_a), cb = tools::searchCounts(*agent_b);
                std::lock_guard<std::mutex> lock(mutex);
                a_think += think_a;
                b_think += think_b;
                a_searched += ca.searched;
                b_searched += cb.searched;
                a_fallbacks += ca.fallbacks;
                b_fallbacks += cb.fallbacks;
                a_counts.add(ca);
                b_counts.add(cb);
                if (clock.tripped) ++time_limited;
                const int done = ++games_done;
                if (done % 50 == 0) {
                    const double secs = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start).count();
                    std::cerr << done << "/" << pairs * 2 << " games: A " << a_wins.load()
                              << " B " << b_wins.load() << " draws " << draws.load() << ", "
                              << secs << "s\n";
                }
            }
            if (a_points > 1.0) ++a_pairs;
            else if (a_points < 1.0) ++b_pairs;
            else ++split_pairs;
            std::lock_guard<std::mutex> lock(mutex);
            sprtUpdate(p, a_points / 2.0);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < opt.threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    const int games = games_done.load();
    const int pairs_played = a_pairs.load() + b_pairs.load() + split_pairs.load();
    const double score = games > 0 ? (a_wins.load() + 0.5 * draws.load()) / games : 0.5;
    if (errors.load() > 0) std::cerr << errors.load() << " games threw (scored as draws)\n";
    std::cerr << "A: " << a_counts.describe() << "\nB: " << b_counts.describe() << "\n";
    std::cout << std::fixed << std::setprecision(4)
              << "{\"games\": " << games << ", \"a_wins\": " << a_wins.load()
              << ", \"b_wins\": " << b_wins.load() << ", \"draws\": " << draws.load()
              << ", \"errors\": " << errors.load() << ", \"a_score\": " << score
              << ", \"pairs\": " << pairs_played << ", \"a_pairs\": " << a_pairs.load()
              << ", \"b_pairs\": " << b_pairs.load() << ", \"split_pairs\": " << split_pairs.load()
              << ", \"a_think_s\": " << a_think << ", \"b_think_s\": " << b_think
              << ", \"a_searched\": " << a_searched << ", \"b_searched\": " << b_searched
              << ", \"a_fallbacks\": " << a_fallbacks << ", \"b_fallbacks\": " << b_fallbacks
              << ", \"time_limited\": " << time_limited;
    if (opt.sprt)
        std::cout << ", \"sprt\": {\"s0\": " << opt.sprt_s0 << ", \"s1\": " << opt.sprt_s1
                  << ", \"llr\": " << sprt_llr << ", \"lower\": " << sprt_lower
                  << ", \"upper\": " << sprt_upper << ", \"pairs_tested\": " << sprt_tested
                  << ", \"decision\": \"" << sprt_decision << "\"}";
    std::cout << "}\n";
    return 0;
}
