/// @file value_data.cpp
/// Records training data for the learned value and policy models.
///
///   riftbound_value_data --out data/values.txt [--games N] [--threads T]
///                        [--seed S] [--decks DIR] [--model FILE] [--guesses N]
///                        [--agent SPEC] [--policy-out FILE]
///
/// Plays games between random pairs of the legal decks in DIR with a mix of
/// agents (the heuristic agent scoring with --model when given: that is how
/// riftbound-trainer's selfplay loop records its current champion), and at the first decision of every turn encodes the position
/// from both players' points of view (ml::extractValueFeatures). That is the
/// point at which HeuristicAgent's playouts call the evaluator. When the
/// game ends each row is labelled 1 if its player won, 0 if they lost;
/// drawn and turn-capped games are dropped.
///
/// One row per line:
///   <label> <game> <turn> <player 1|2> <my deck> <opp deck> <index>:<value> ...
/// Deck numbers index the list written to <out>.decks; feature names go to
/// <out>.names (one per index).
///
/// <out>.search has one line per row: the heuristic agent's search estimate
/// of its player's winning chances that turn (the best action's score at the
/// player's first searched decision of the turn), or "nan" where the player
/// did not search that turn or scores are not probabilities (no model).
/// riftbound-trainer blends it into the label (value_gbt --search-blend).
///
/// --agent SPEC replaces the heuristic agent (a tools/tool_common.h spec,
/// e.g. heuristic:model=M,policy=P); --model / --guesses then do not apply.
/// --policy-out FILE records the heuristic agents' searched decisions — and
/// the follow-ups answered from their plans — one line per legal action:
///   <decision> <score|nan> <chosen 0/1> <kind> <game> <turn> <index>:<value> ...
/// (ml::extractPolicyFeatures; names to FILE.names), for policy_gbt.py.

#include "tool_common.h"

#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/policy_features.h"
#include "ml/value_features.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace riftbound;

namespace {

struct Options {
    std::string out;
    std::string decks_dir = "decks";
    int games = 1000;
    int threads = 0;
    uint64_t seed = 1;
    std::string model;  // learned value model for the heuristic agent, if any
    int guesses = 0;    // heuristic determinizations; 0 = HeuristicConfig's default
    std::string agent;  // full heuristic spec, replacing --model / --guesses
    std::string policy_out;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--out") o.out = next();
        else if (a == "--decks") o.decks_dir = next();
        else if (a == "--games") o.games = std::stoi(next());
        else if (a == "--threads") o.threads = std::stoi(next());
        else if (a == "--seed") o.seed = std::stoull(next());
        else if (a == "--model") o.model = next();
        else if (a == "--guesses") o.guesses = std::stoi(next());
        else if (a == "--agent") o.agent = next();
        else if (a == "--policy-out") o.policy_out = next();
        else throw std::runtime_error("unknown argument " + a);
    }
    if (o.out.empty()) throw std::runtime_error("--out is required");
    if (o.threads <= 0) o.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return o;
}

// Matchups and their weights: mostly the heuristic agent, whose positions the
// model will score, plus weaker agents for positions it would not reach.
enum class Kind { Heuristic, Scripted, Random };
struct Matchup { Kind p1, p2; int weight; };
constexpr Matchup kMatchups[] = {
    {Kind::Heuristic, Kind::Heuristic, 4},
    {Kind::Heuristic, Kind::Scripted, 1}, {Kind::Scripted, Kind::Heuristic, 1},
    {Kind::Scripted, Kind::Scripted, 2},
    {Kind::Heuristic, Kind::Random, 1}, {Kind::Random, Kind::Heuristic, 1},
};

std::unique_ptr<AgentInterface> makeAgent(Kind kind, const tools::ToolAgentSpec& heuristic,
                                          const CardDB& db, const CardRegistry& reg,
                                          uint64_t seed) {
    tools::ToolAgentSpec spec = heuristic;
    if (kind == Kind::Scripted) spec.kind = "scripted";
    else if (kind == Kind::Random) spec.kind = "random";
    return tools::makeToolAgent(spec, db, reg, seed);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"
                  << "usage: riftbound_value_data --out FILE [--games N] [--threads T] "
                     "[--seed S] [--decks DIR] [--model FILE] [--guesses N] [--agent SPEC] "
                     "[--policy-out FILE]\n";
        return 2;
    }

    CardRegistry registry;
    registry.loadAll();
    CardDB db;
    db.buildFromClasses(registry);

    tools::ToolAgentSpec heuristic;
    try {
        if (!opt.agent.empty()) {
            heuristic = tools::parseToolAgent(opt.agent);
            if (heuristic.kind != "heuristic") throw std::runtime_error("--agent must be a heuristic spec");
        } else if (!opt.model.empty()) {
            heuristic.heuristic.model = GbtModel::load(opt.model);
        }
    } catch (const std::exception& e) {
        std::cerr << "bad agent / model: " << e.what() << "\n";
        return 1;
    }
    if (opt.guesses > 0 && opt.agent.empty()) heuristic.heuristic.guesses = opt.guesses;
    // Search scores are win probabilities only when a model scores them.
    const bool scores_are_probabilities = heuristic.heuristic.model != nullptr;

    const tools::DeckSet deck_set = tools::loadLegalDecks(opt.decks_dir, db);
    const auto& deck_names = deck_set.names;
    const auto& decks = deck_set.decks;
    if (decks.size() < 2) {
        std::cerr << "need at least two legal decks in " << opt.decks_dir << "\n";
        return 1;
    }

    {
        std::ofstream names(opt.out + ".names");
        for (const auto& n : ml::valueFeatureNames(db)) names << n << "\n";
        std::ofstream deck_list(opt.out + ".decks");
        for (const auto& n : deck_names) deck_list << n << "\n";
    }
    std::ofstream out(opt.out), search_out(opt.out + ".search");
    if (!out || !search_out) {
        std::cerr << "cannot write " << opt.out << "\n";
        return 1;
    }
    std::ofstream policy_out;
    if (!opt.policy_out.empty()) {
        std::ofstream names(opt.policy_out + ".names");
        for (const auto& n : ml::policyFeatureNames(db)) names << n << "\n";
        policy_out.open(opt.policy_out);
        if (!policy_out) {
            std::cerr << "cannot write " << opt.policy_out << "\n";
            return 1;
        }
    }

    int total_weight = 0;
    for (const auto& m : kMatchups) total_weight += m.weight;

    std::atomic<int> next_game{0}, games_done{0}, games_dropped{0};
    std::atomic<long> rows_written{0}, next_decision{0}, policy_rows{0};
    std::mutex out_mutex;
    const auto start = std::chrono::steady_clock::now();

    const auto worker = [&] {
        const ScriptedAgent scripted(db);
        for (int g = next_game++; g < opt.games; g = next_game++) {
            std::mt19937_64 rng(opt.seed * 1000003ULL + static_cast<uint64_t>(g));
            std::uniform_int_distribution<size_t> pick_deck(0, decks.size() - 1);
            const size_t d1 = pick_deck(rng), d2 = pick_deck(rng);
            int w = std::uniform_int_distribution<int>(0, total_weight - 1)(rng);
            const Matchup* m = &kMatchups[0];
            for (const auto& candidate : kMatchups) {
                if (w < candidate.weight) { m = &candidate; break; }
                w -= candidate.weight;
            }
            const uint64_t game_seed = (rng() & 0x7FFFFFFFULL) | 1;

            auto a1 = makeAgent(m->p1, heuristic, db, registry, rng());
            auto a2 = makeAgent(m->p2, heuristic, db, registry, rng());
            EventBus bus;
            GameEngine engine(db, bus, registry);

            // Rows of this game, labelled once the winner is known.
            struct Row {
                PlayerId player;
                int turn;
                ml::SparseRow features;
                double search = std::nan("");
            };
            std::vector<Row> rows;
            int last_turn = -1;
            engine.on_decision = [&](const GameState& s, const std::vector<Intent>&, const Intent&) {
                if (s.turn.phase == TurnPhase::Setup || s.turn.phase == TurnPhase::Mulligan) return;
                if (s.turn.turn_number == last_turn) return;
                last_turn = s.turn.turn_number;
                for (PlayerId p : {PlayerId::Player1, PlayerId::Player2}) {
                    rows.push_back({p, s.turn.turn_number, {}});
                    ml::extractValueFeatures(s, p, rows.back().features);
                }
            };

            // Search results of the heuristic seats: a player's first
            // searched decision of a turn sets that turn's row estimate, and
            // every one becomes policy rows.
            std::ostringstream policy_buf;
            const auto observe = [&](const GameState& s, const std::vector<Intent>& legal,
                                     const std::vector<double>& scores) {
                const PlayerId p = legal[0].player;
                int chosen = -1;
                for (size_t i = 0; i < scores.size(); ++i)
                    if (!std::isnan(scores[i]) && (chosen < 0 || scores[i] > scores[chosen]))
                        chosen = static_cast<int>(i);
                if (chosen < 0) return;
                if (scores_are_probabilities && s.turn.phase != TurnPhase::Setup &&
                    s.turn.phase != TurnPhase::Mulligan) {
                    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                        if (it->turn != s.turn.turn_number) break;
                        if (it->player != p) continue;
                        if (std::isnan(it->search)) it->search = std::clamp(scores[chosen], 0.0, 1.0);
                        break;
                    }
                }
                if (!policy_out.is_open()) return;
                const long decision = next_decision++;
                const char* kind = ml::policyDecisionKind(s, legal);
                const int scripted_pick = scripted.selectIndex(s, legal);
                ml::SparseRow position, row;
                ml::extractValueFeatures(s, p, position);
                for (size_t i = 0; i < legal.size(); ++i) {
                    ml::extractPolicyFeatures(s, db, legal, static_cast<int>(i), position, kind,
                                              scripted_pick, row);
                    policy_buf << decision << ' ';
                    if (std::isnan(scores[i])) policy_buf << "nan";
                    else policy_buf << scores[i];
                    policy_buf << ' ' << (static_cast<int>(i) == chosen ? 1 : 0) << ' ' << kind
                               << ' ' << g << ' ' << s.turn.turn_number;
                    for (const auto& [k, v] : row) policy_buf << ' ' << k << ':' << v;
                    policy_buf << '\n';
                }
            };
            for (AgentInterface* a : {a1.get(), a2.get()})
                if (auto* h = dynamic_cast<HeuristicAgent*>(a)) h->setSearchObserver(observe);

            GameResult result;
            try {
                result = engine.runGame(decks[d1], decks[d2], *a1, *a2, game_seed);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(out_mutex);
                std::cerr << "game " << g << " threw: " << e.what() << "\n";
                ++games_dropped;
                continue;
            }
            if (result.winner == PlayerId::None) {
                ++games_dropped;
            } else {
                std::ostringstream buf, search_buf;
                for (const Row& r : rows) {
                    const bool p1 = r.player == PlayerId::Player1;
                    buf << (r.player == result.winner ? 1 : 0) << ' ' << g << ' ' << r.turn << ' '
                        << (p1 ? 1 : 2) << ' ' << (p1 ? d1 : d2) << ' ' << (p1 ? d2 : d1);
                    for (const auto& [i, v] : r.features) buf << ' ' << i << ':' << v;
                    buf << '\n';
                    if (std::isnan(r.search)) search_buf << "nan\n";
                    else search_buf << r.search << '\n';
                }
                const std::string policy_text = policy_buf.str();
                std::lock_guard<std::mutex> lock(out_mutex);
                out << buf.str();
                search_out << search_buf.str();
                rows_written += static_cast<long>(rows.size());
                if (policy_out.is_open()) {
                    policy_out << policy_text;
                    policy_rows += std::count(policy_text.begin(), policy_text.end(), '\n');
                }
            }

            const int done = ++games_done;
            if (done % 500 == 0) {
                const double secs = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count();
                std::lock_guard<std::mutex> lock(out_mutex);
                std::cerr << done << "/" << opt.games << " games, " << rows_written.load()
                          << " rows, " << secs << "s\n";
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < opt.threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    std::cerr << "done: " << games_done.load() << " games (" << games_dropped.load()
              << " dropped), " << rows_written.load() << " rows -> " << opt.out;
    if (policy_out.is_open())
        std::cerr << ", " << policy_rows.load() << " policy rows -> " << opt.policy_out;
    std::cerr << "\n";
    return 0;
}
