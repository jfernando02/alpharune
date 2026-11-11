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
///
/// --nn-out PREFIX records the same positions for the neural model as a
/// binary shard (ml/obs_io.h): every turn's first decision from both
/// players' views (value only), and every searched decision with its legal
/// actions and their scores. With it --out may be left out.
///
/// --agent also takes policy:policy=FILE[,temp=X,eps=X] (agents/policy_agent):
/// the policy with no search, thousands of games an hour, for value data.
/// --self-play makes every game that agent against itself (no ScriptedAgent
/// or random seats).

#include "tool_common.h"

#include "agents/policy_agent.h"

#include "core/events.h"
#include "engine/game_engine.h"
#include "ml/obs_io.h"
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
    std::string nn_out;
    bool self_play = false;
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
        else if (a == "--nn-out") o.nn_out = next();
        else if (a == "--self-play") o.self_play = true;
        else throw std::runtime_error("unknown argument " + a);
    }
    if (o.out.empty() && o.nn_out.empty()) throw std::runtime_error("--out or --nn-out is required");
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

/// A JSON string literal of `text`.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

/// The shard's description for the trainer: versions, record sizes (so a
/// reader with another layout fails loudly), feature and deck names.
void writeShardMeta(const std::string& path, const CardDB& db,
                    const std::vector<std::string>& deck_names, const Options& opt) {
    std::ofstream meta(path);
    if (!meta) throw std::runtime_error("cannot write " + path);
    const auto list = [&](const char* key, const std::vector<std::string>& names) {
        meta << "  \"" << key << "\": [";
        for (size_t i = 0; i < names.size(); ++i) meta << (i ? ", " : "") << quoted(names[i]);
        meta << "],\n";
    };
    const std::vector<std::string> policy_names = ml::policyFeatureNames(db);
    const std::vector<std::string> action_names(
        policy_names.begin() + ml::kPolicyActionOffset,
        policy_names.begin() + ml::kPolicyActionOffset + ml::kObsActionDim);
    std::vector<std::string> cards(ml::kObsCardVocab, "unused");
    for (const auto& [id, def] : db.all())
        if (id >= 1 && id < static_cast<CardDefId>(ml::kObsCardVocab)) cards[id] = def.name;
    meta << "{\n"
         << "  \"obs_version\": " << ml::kObsVersion << ",\n"
         << "  \"globals\": " << ml::kObsGlobalDim << ",\n"
         << "  \"entity_features\": " << ml::kObsEntityDim << ",\n"
         << "  \"action_features\": " << ml::kObsActionDim << ",\n"
         << "  \"card_vocab\": " << ml::kObsCardVocab << ",\n"
         << "  \"groups\": " << ml::kObsGroupSlots << ",\n"
         << "  \"pools\": " << ml::kObsPoolCount << ",\n"
         << "  \"fixed_pools\": " << ml::kObsPoolBattlefield << ",\n"
         << "  \"max_battlefields\": " << ml::kObsMaxBattlefields << ",\n"
         << "  \"max_chosen\": " << ml::kObsMaxChosen << ",\n"
         << "  \"max_moved\": " << ml::kObsMaxMoved << ",\n"
         << "  \"bags\": " << static_cast<int>(ml::ObsBag::Count) << ",\n"
         << "  \"battlefield_group\": " << static_cast<int>(ml::ObsGroup::Battlefield) << ",\n"
         << "  \"first_single_group\": " << static_cast<int>(ml::ObsGroup::MyLegend) << ",\n"
         << "  \"pos_size\": " << sizeof(ml::PosRecord) << ",\n"
         << "  \"ent_size\": " << sizeof(ml::EntRecord) << ",\n"
         << "  \"act_size\": " << sizeof(ml::ActRecord) << ",\n"
         << "  \"bag_size\": " << sizeof(ml::BagRecord) << ",\n";
    list("kinds", ml::obsKindNames());
    list("global_names", ml::obsGlobalNames());
    list("entity_names", ml::obsEntityNames());
    list("action_names", action_names);
    list("cards", cards);
    list("decks", deck_names);
    meta << "  \"agent\": " << quoted(opt.agent) << ",\n"
         << "  \"seed\": " << opt.seed << ",\n"
         << "  \"games\": " << opt.games << "\n}\n";
}

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
            if (heuristic.kind != "heuristic" && heuristic.kind != "policy")
                throw std::runtime_error("--agent must be a heuristic or policy spec");
        } else if (!opt.model.empty()) {
            heuristic.heuristic.model = loadPositionModel(opt.model);
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

    const bool text_out = !opt.out.empty();
    std::ofstream out, search_out;
    if (text_out) {
        std::ofstream names(opt.out + ".names");
        for (const auto& n : ml::valueFeatureNames(db)) names << n << "\n";
        std::ofstream deck_list(opt.out + ".decks");
        for (const auto& n : deck_names) deck_list << n << "\n";
        out.open(opt.out);
        search_out.open(opt.out + ".search");
        if (!out || !search_out) {
            std::cerr << "cannot write " << opt.out << "\n";
            return 1;
        }
    }
    std::unique_ptr<ml::ObsShardWriter> nn_out;
    if (!opt.nn_out.empty()) {
        try {
            nn_out = std::make_unique<ml::ObsShardWriter>(opt.nn_out);
            writeShardMeta(opt.nn_out + ".meta.json", db, deck_names, opt);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
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
    // Policy agents that hit their per-turn decision guard (a repeatable action).
    std::atomic<long> capped_games{0}, capped_decisions{0};
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
            if (opt.self_play) m = &kMatchups[0];
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
            // The same positions for the neural model, plus its searched
            // decisions; turn_sample is each player's value-only sample of
            // the current turn.
            std::vector<ml::ObsSample> samples;
            int turn_sample[2] = {-1, -1};
            int last_turn = -1;
            engine.on_decision = [&](const GameState& s, const std::vector<Intent>&, const Intent&) {
                if (s.turn.phase == TurnPhase::Setup || s.turn.phase == TurnPhase::Mulligan) return;
                if (s.turn.turn_number == last_turn) return;
                last_turn = s.turn.turn_number;
                for (PlayerId p : {PlayerId::Player1, PlayerId::Player2}) {
                    if (text_out) {
                        rows.push_back({p, s.turn.turn_number, {}});
                        ml::extractValueFeatures(s, p, rows.back().features);
                    }
                    if (nn_out) {
                        turn_sample[playerIndex(p)] = static_cast<int>(samples.size());
                        samples.emplace_back();
                        samples.back().player = p;
                        samples.back().turn = s.turn.turn_number;
                        ml::encodeObs(s, p, db, samples.back().obs);
                    }
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
                const char* kind = ml::policyDecisionKind(s, legal);
                const int scripted_pick = scripted.selectIndex(s, legal);
                if (nn_out) {
                    const bool in_game = s.turn.phase != TurnPhase::Setup && s.turn.phase != TurnPhase::Mulligan;
                    const double estimate = scores_are_probabilities
                        ? std::clamp(scores[chosen], 0.0, 1.0) : std::nan("");
                    const int ts = turn_sample[playerIndex(p)];
                    if (in_game && ts >= 0 && samples[ts].turn == s.turn.turn_number &&
                        std::isnan(samples[ts].search_value))
                        samples[ts].search_value = estimate;
                    samples.emplace_back();
                    ml::ObsSample& sample = samples.back();
                    sample.player = p;
                    sample.turn = s.turn.turn_number;
                    sample.kind = ml::obsKindIndex(kind);
                    sample.chosen = chosen;
                    sample.search_value = estimate;
                    sample.scores = scores;
                    ml::encodeObs(s, p, db, sample.obs);
                    ml::encodeActions(s, db, legal, sample.obs, kind, scripted_pick, sample.actions);
                }
                if (!policy_out.is_open()) return;
                const long decision = next_decision++;
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
            long capped = 0;
            for (AgentInterface* a : {a1.get(), a2.get()})
                if (auto* p = dynamic_cast<PolicyAgent*>(a)) capped += p->cappedDecisions();
            if (capped > 0) { ++capped_games; capped_decisions += capped; }
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
                if (text_out) {
                    out << buf.str();
                    search_out << search_buf.str();
                }
                rows_written += static_cast<long>(rows.size());
                if (nn_out)
                    for (const ml::ObsSample& sample : samples) {
                        const bool p1 = sample.player == PlayerId::Player1;
                        nn_out->write(sample, static_cast<uint32_t>(g), sample.player == result.winner ? 1 : 0,
                                      static_cast<int>(p1 ? d1 : d2), static_cast<int>(p1 ? d2 : d1));
                    }
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
    if (nn_out)
        std::cerr << ", " << nn_out->positions() << " positions (" << nn_out->actions()
                  << " actions) -> " << opt.nn_out;
    if (policy_out.is_open())
        std::cerr << ", " << policy_rows.load() << " policy rows -> " << opt.policy_out;
    std::cerr << "\n";
    std::cerr << "policy decision guard: " << capped_games.load() << " games, "
              << capped_decisions.load() << " decisions passed" << std::endl;
    return 0;
}
