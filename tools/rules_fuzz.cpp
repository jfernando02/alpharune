/// riftbound_rules_fuzz — plays many games over the deck pairs with random,
/// scripted and policy agents and checks rule invariants after every decision.
/// The checks live here (a checker over the public state and events), not in
/// card or engine code. Every violation prints the game number, its seed, the
/// decks, the agents and the decision index; `--only-game G` replays one game.
///
///   riftbound_rules_fuzz [--games N] [--threads T] [--seed S] [--decks DIR]
///       [--agents SPEC;SPEC;...] [--determinism-every K] [--copy-every K]
///       [--all-legal-every K] [--only-game G] [--max-print N]
///       [--scope FILE] [--usage FILE]
///
/// --agents separates agent specs with ';' (a spec may contain ','); each game
/// takes the next ordered pair of them. --scope writes the in-scope cards of
/// the deck lists (CSV) and exits. --usage writes per-card usage statistics.
///
/// Checks (see docs/rules-audit/fuzz-findings.md for what each one means):
///   zone      every object is in exactly one zone list that agrees with it
///   conserve  each player's non-token object count never changes
///   negative  no negative energy, power, XP, score, damage, buffs
///   token     tokens never sit in a hand, deck, trash or banishment list
///   exhaust   an exhaust-cost ability is never activated from an exhausted object
///   twice     an exhaust-cost ability is not used more often in a turn than its
///             object was readied plus one; a "once per turn" one not twice
///   score     scores change only with a scoring event, and the game ends at
///             the victory score
///   bothunits no battlefield holds units of both players at a clean main-phase
///             decision (cleanup must have started a showdown or combat)
///   pool      rune pools are empty at the first main-phase decision of a turn
///   longturn  more than 200 decisions in one turn; turncap the turn limit hit
///   throw     an enumerated legal intent threw when applied
///   determinism  the same seed and choices give the same states twice
///   copy      a snapshot copy offers the same decisions and, stepped with the
///             same choice, reaches the same state (no random event in between)

#include "tool_common.h"

#include "agents/agent_interface.h"
#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "core/events.h"
#include "engine/game_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace riftbound;

namespace {

struct Options {
    int games = 1000;
    int threads = 2;
    uint64_t seed = 1;
    std::string decks_dir = "decks";
    std::vector<std::string> agents = {"random", "scripted"};
    int determinism_every = 8;   // replay every K-th game and compare states
    int copy_every = 7;          // copy check at every K-th clean decision
    int all_legal_every = 50;    // apply every legal intent on copies at every K-th clean decision
    int only_game = -1;
    int max_print = 3;           // violations printed per (check, key)
    bool trace = false;          // print the engine's trace log (use with --only-game)
    std::string scope_out, usage_out;
};

std::vector<std::string> splitSemis(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string locStr(const std::optional<LocationId>& l) {
    if (!l) return "-";
    if (auto* b = std::get_if<BaseLocation>(&*l)) return "base" + std::to_string(static_cast<int>(b->player));
    return "bf" + std::to_string(std::get<BattlefieldLocation>(*l).id);
}

/// Canonical text of the state the rules care about, for determinism and copy checks.
/// `deck_order`: false lists deck contents sorted, for the copy check -- a copy has its own
/// engine RNG (not part of GameState), so a random reorder of a deck legitimately differs.
std::string dumpState(const GameState& s, bool deck_order = true) {
    std::ostringstream o;
    o << "turn " << s.turn.turn_number << " ph " << static_cast<int>(s.turn.phase) << " tp "
      << static_cast<int>(s.turn.turn_player) << " ns " << static_cast<int>(s.turn.ns_state) << " oc "
      << static_cast<int>(s.turn.oc_state) << " over " << s.game_over << " w " << static_cast<int>(s.winner) << "\n";
    for (int p = 0; p < 2; ++p) {
        const auto& ps = s.players[p];
        o << "P" << p << " score " << ps.score << " xp " << ps.xp << " energy " << ps.rune_pool.energy << " power";
        for (int d = 0; d < static_cast<int>(Domain::Count); ++d) o << ' ' << ps.rune_pool.power[d];
        o << " any " << ps.rune_pool.universal_power << " burned " << ps.burned_out << "\n";
        const auto list = [&](const char* name, std::vector<GameObjectId> v) {
            if (!deck_order && (std::string(name) == "deck" || std::string(name) == "runes"))
                std::sort(v.begin(), v.end());
            o << "  " << name;
            for (auto id : v) o << ' ' << id;
            o << "\n";
        };
        list("hand", ps.hand); list("deck", ps.main_deck); list("runes", ps.rune_deck);
        list("trash", ps.trash); list("banish", ps.banishment);
        o << "  champ " << ps.champion_zone << " legend " << ps.legend_zone << "\n";
    }
    std::vector<GameObjectId> ids;
    for (const auto& [id, obj] : s.objects) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) {
        const auto& g = s.objects.at(id);
        o << "o" << id << " d" << g.card_def_id << " own " << static_cast<int>(g.owner) << " ctl "
          << static_cast<int>(g.controller) << " z " << static_cast<int>(g.zone) << " at " << locStr(g.location)
          << " ex " << g.is_exhausted << " st " << g.is_stunned << " hid " << g.is_hidden << " dmg " << g.damage_marked
          << " m " << g.current_might << " buff " << g.buff_count << " kw " << g.keywords.bits << " att "
          << (g.attached_to ? static_cast<long>(*g.attached_to) : -1L) << " tok " << g.is_token << " emp " << g.empowered;
        std::vector<std::pair<std::string, int>> c(g.card_counters.begin(), g.card_counters.end());
        std::sort(c.begin(), c.end());
        for (const auto& [k, v] : c) o << ' ' << k << '=' << v;
        o << "\n";
    }
    for (const auto& bf : s.battlefields) {
        o << "bf" << bf.id << " ctl " << (bf.controller ? static_cast<int>(*bf.controller) : -1) << " contested "
          << bf.is_contested << " fd";
        for (auto id : bf.facedown) o << ' ' << id;
        o << "\n";
    }
    o << "chain";
    for (const auto& it : s.chain.items) o << ' ' << it.source;
    o << "\n";
    return o.str();
}

std::string firstDiff(const std::string& a, const std::string& b) {
    std::istringstream ia(a), ib(b);
    std::string la, lb;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(ia, la));
        const bool gb = static_cast<bool>(std::getline(ib, lb));
        if (!ga && !gb) return "";
        if (!ga || !gb || la != lb) return "'" + (ga ? la : "<end>") + "' vs '" + (gb ? lb : "<end>") + "'";
    }
}

bool isActivation(IntentType t) {
    return t == IntentType::ActivateAbility || t == IntentType::ActivateActionAbility ||
           t == IntentType::ActivateReactionAbility;
}

// ── Violation sink ──────────────────────────────────────────────────────────

struct Sink {
    std::mutex mu;
    int max_print = 3;
    std::map<std::pair<std::string, std::string>, long> counts;  // (check, key) -> n
    std::map<std::string, long> by_check;

    void report(const std::string& check, const std::string& key, const std::string& where,
                const std::string& detail) {
        std::lock_guard<std::mutex> lock(mu);
        const long n = ++counts[{check, key}];
        ++by_check[check];
        if (n <= max_print)
            std::cout << "VIOLATION " << check << " [" << key << "] " << where << ": " << detail << std::endl;
    }
};

// ── Per-card usage statistics (step C) ──────────────────────────────────────

struct CardUsage {
    long games_in_deck = 0, plays = 0, activations = 0, triggers = 0;
    long games_played = 0, wins_when_played = 0;
    long max_activations_turn = 0, max_triggers_turn = 0, max_plays_turn = 0;
    double expected_wins_when_played = 0;  // the deck's own win rate summed over those games
};

struct Usage {
    std::mutex mu;
    std::map<CardDefId, CardUsage> cards;
    std::map<std::string, std::pair<long, long>> deck_wl;  // deck -> (games, wins)
};

// ── One game ────────────────────────────────────────────────────────────────

struct Shared {
    Shared(const Options& o, const CardDB& d, const CardRegistry& r, const tools::DeckSet& k)
        : opt(o), db(d), reg(r), decks(k) {}
    const Options& opt;
    const CardDB& db;
    const CardRegistry& reg;
    const tools::DeckSet& decks;
    std::vector<tools::ToolAgentSpec> specs;
    Sink sink;
    Usage usage;
    std::atomic<long> decisions{0}, copies{0}, legal_checked{0}, replays{0};
    std::map<std::string, long> endings;
    std::map<std::string, long> entered_ready_units, entered_exhausted_gear;  // card name -> n
    std::mutex misc_mu;
};

struct GameSetup {
    int g;
    size_t d1, d2;
    size_t s1, s2;
    uint64_t game_seed, agent_seed1, agent_seed2, copy_seed;
};

GameSetup setupFor(const Options& opt, const Shared& sh, int g) {
    std::mt19937_64 rng(opt.seed * 1000003ULL + static_cast<uint64_t>(g));
    GameSetup su{};
    su.g = g;
    const size_t n = sh.decks.decks.size();
    // Walk the ordered deck pairs so every pair comes up; the seed varies the rest.
    const size_t pair = static_cast<size_t>(g) % (n * n);
    su.d1 = pair / n;
    su.d2 = pair % n;
    const size_t a = sh.specs.size();
    const size_t ap = (static_cast<size_t>(g) / (n * n) + static_cast<size_t>(g)) % (a * a);
    su.s1 = ap / a;
    su.s2 = ap % a;
    su.game_seed = (rng() & 0x7FFFFFFFULL) | 1;
    su.agent_seed1 = rng();
    su.agent_seed2 = rng();
    su.copy_seed = rng() | 1;
    return su;
}

class GameChecker {
public:
    GameChecker(Shared& sh, const GameSetup& su) : sh_(sh), su_(su) {}

    std::string where(const GameState& s) const {
        std::ostringstream o;
        o << "game " << su_.g << " seed " << su_.game_seed << " decks " << sh_.decks.names[su_.d1] << ","
          << sh_.decks.names[su_.d2] << " agents " << su_.s1 << "," << su_.s2 << " decision " << decision_
          << " turn " << s.turn.turn_number;
        return o.str();
    }

    void run() {
        const auto& opt = sh_.opt;
        EventBus bus;
        GameEngine engine(sh_.db, bus, sh_.reg);
        engine_ = &engine;
        connect(bus);
        auto a1 = tools::makeToolAgent(sh_.specs[su_.s1], sh_.db, sh_.reg, su_.agent_seed1);
        auto a2 = tools::makeToolAgent(sh_.specs[su_.s2], sh_.db, sh_.reg, su_.agent_seed2);
        std::mt19937_64 check_rng(su_.copy_seed);
        const bool record = opt.determinism_every > 0 && su_.g % opt.determinism_every == 0;

        StepResult step;
        try {
            step = engine.beginGame(sh_.decks.decks[su_.d1], sh_.decks.decks[su_.d2], su_.game_seed);
        } catch (const std::exception& e) {
            sh_.sink.report("throw", "beginGame", where(engine.state()), e.what());
            return;
        }
        int last_turn = -1;
        long in_turn = 0, clean_points = 0;
        while (step.kind == StepKind::NeedDecision) {
            const GameState& s = engine.state();
            if (s.turn.turn_number != last_turn) {
                last_turn = s.turn.turn_number;
                in_turn = 0;
                uses_.clear();
                readies_.clear();
                flushTurnUsage();
            }
            if (++in_turn == 201)
                sh_.sink.report("longturn", "turn", where(s), "more than 200 decisions in one turn");
            checkState(s);
            trackReadies(s);
            if (record) {
                trace_.push_back(dumpState(s));
                legal_sizes_.push_back(step.legal.size());
            }

            AgentInterface& agent = step.legal[0].player == PlayerId::Player1 ? *a1 : *a2;
            Intent pick = agent.selectAction(s, step.legal);
            int idx = -1;
            for (size_t i = 0; i < step.legal.size(); ++i)
                if (step.legal[i] == pick) { idx = static_cast<int>(i); break; }
            if (idx < 0) {
                sh_.sink.report("agent", "unlisted", where(s), "agent chose an intent not in the legal list: " + pick.describe());
                idx = 0;
            }
            checkActivation(s, step.legal[idx]);
            noteUsage(s, step.legal[idx]);

            const bool clean = isCleanResumePoint(s) || isCleanShowdownPoint(s) || isCleanChainPoint(s);
            std::unique_ptr<EventBus> copy_bus;
            std::unique_ptr<GameEngine> copy;
            if (clean) {
                ++clean_points;
                if (opt.all_legal_every > 0 && clean_points % opt.all_legal_every == 0)
                    tryAllLegal(s, step);
                if (opt.copy_every > 0 && clean_points % opt.copy_every == 0) {
                    copy_bus = std::make_unique<EventBus>();
                    copy = std::make_unique<GameEngine>(sh_.db, *copy_bus, sh_.reg);
                    try {
                        StepResult cs = copy->resumeFromSnapshot(s, check_rng() | 1);
                        if (cs.legal != step.legal) {
                            sh_.sink.report("copy", "legal", where(s),
                                            "copy offers " + std::to_string(cs.legal.size()) + " decisions, original " +
                                                std::to_string(step.legal.size()));
                            copy.reset();
                        } else {
                            copy->applyChoice(idx);
                        }
                    } catch (const std::exception& e) {
                        sh_.sink.report("copy", "throw", where(s), e.what());
                        copy.reset();
                    }
                    ++sh_.copies;
                }
            }

            chosen_.push_back(idx);
            random_events_ = 0;
            try {
                step = engine.applyChoice(idx);
            } catch (const std::exception& e) {
                sh_.sink.report("throw", step.legal[idx].describe().substr(0, 40), where(s),
                                step.legal[idx].describe() + " threw: " + e.what());
                return;
            }
            ++decision_;
            ++sh_.decisions;
            if (copy && random_events_ == 0) {
                const std::string a = dumpState(engine.state(), false),
                                  b = dumpState(copy->state(), false);
                if (a != b) sh_.sink.report("copy", "state", where(engine.state()), firstDiff(a, b));
            }
        }

        const GameState& s = engine.state();
        flushTurnUsage();
        checkEnd(s);
        noteResult(s);
        if (record) replay();
    }

private:
    void connect(EventBus& bus) {
        bus.on_score.connect([this](const ScoreEvent& e) { known_score_[static_cast<int>(e.player) - 1] = e.new_score; });
        bus.on_points_gained.connect([this](const PointsGainedEvent& e) {
            known_score_[static_cast<int>(e.player) - 1] = e.new_score;
        });
        // Rune pools empty at the start of each Main Phase (CR 167, 316.3): checked as the Main Phase
        // begins, before anyone acts (a pool may hold Energy again later in the turn).
        bus.on_phase_changed.connect([this](const PhaseChangedEvent& e) {
            if (e.new_phase == TurnPhase::MainPhase && engine_) checkPoolsEmpty(engine_->state());
        });
        bus.on_unit_readied.connect([this](const UnitReadiedEvent& e) { ++readies_[e.object]; });
        bus.on_entered_board.connect([this](const EnteredBoardEvent& e) { entered_.push_back(e.object); });
        bus.on_card_played.connect([this](const CardPlayedEvent& e) {
            const auto& s = engine_->state();
            if (!s.objectExists(e.object)) return;
            const CardDefId d = s.getObject(e.object).card_def_id;
            ++turn_plays_[d];
            played_[static_cast<int>(e.player) - 1].insert(d);
        });
        bus.on_chain_item_finalized.connect([this](const ChainItemFinalizedEvent& e) {
            const auto& s = engine_->state();
            for (const auto& it : s.chain.items) {
                if (it.id != e.item || !it.is_ability || it.is_activated_ability) continue;
                if (!s.objectExists(e.source)) return;
                ++turn_triggers_[s.getObject(e.source).card_def_id];
            }
        });
        // Shuffles and other random events make a snapshot copy diverge legitimately.
        bus.on_log.connect([this](const LogEvent& e) {
            if (sh_.opt.trace) std::cout << "  [" << decision_ << "] " << e.message << '\n';
            const std::string m = lower(e.message);
            if (m.find("shuffl") != std::string::npos || m.find("random") != std::string::npos) ++random_events_;
        });
    }

    void checkState(const GameState& s) {
        const bool settled = isCleanResumePoint(s) || isCleanShowdownPoint(s) || isCleanChainPoint(s);
        if (settled) checkZones(s);
        checkScores(s);
        if (s.chain.exists() && !s.turn.isClosedState())
            sh_.sink.report("state", "open with a chain", where(s), "a Chain exists but the turn is Open (CR 309.1)");
    }

    // Zone lists, tokens and object counts, at settled points only: while a cost is paid or an
    // item resolves, a card may sit between lists by design.
    void checkZones(const GameState& s) {
        // zone lists
        std::unordered_map<GameObjectId, int> listed;
        const auto check_list = [&](PlayerId p, const std::vector<GameObjectId>& v, ZoneType z, const char* name) {
            for (auto id : v) {
                ++listed[id];
                if (!s.objectExists(id)) {
                    sh_.sink.report("zone", std::string(name) + " missing", where(s), "list holds a missing object");
                    continue;
                }
                const auto& o = s.getObject(id);
                if (o.zone != z)
                    sh_.sink.report("zone", std::string(name) + " " + o.name, where(s),
                                    o.name + " is in the " + name + " list but its zone is " + std::to_string(static_cast<int>(o.zone)));
                if (o.owner != p)
                    sh_.sink.report("zone", std::string(name) + " owner " + o.name, where(s),
                                    o.name + " is in the other player's " + name + " list");
                if (o.is_token)
                    sh_.sink.report("token", std::string(name) + " " + o.name, where(s),
                                    "token " + o.name + " is in a " + name + " list");
                if (o.location && z != ZoneType::ChampionZone && z != ZoneType::LegendZone)
                    sh_.sink.report("zone", std::string(name) + " location " + o.name, where(s),
                                    o.name + " in " + name + " still has a board location " + locStr(o.location));
            }
        };
        for (int p = 0; p < 2; ++p) {
            const auto& ps = s.players[p];
            const PlayerId pid = ps.id;
            check_list(pid, ps.hand, ZoneType::Hand, "hand");
            check_list(pid, ps.main_deck, ZoneType::MainDeck, "deck");
            check_list(pid, ps.rune_deck, ZoneType::RuneDeck, "rune deck");
            check_list(pid, ps.trash, ZoneType::Trash, "trash");
            check_list(pid, ps.banishment, ZoneType::Banishment, "banishment");
            if (ps.champion_zone != kInvalidId) check_list(pid, {ps.champion_zone}, ZoneType::ChampionZone, "champion zone");
            if (ps.legend_zone != kInvalidId) check_list(pid, {ps.legend_zone}, ZoneType::LegendZone, "legend zone");
        }
        std::set<GameObjectId> on_chain;
        for (const auto& it : s.chain.items) on_chain.insert(it.source);
        if (s.chain.resuming) on_chain.insert(s.chain.resuming->source);
        std::set<GameObjectId> facedown;
        for (const auto& bf : s.battlefields) for (auto id : bf.facedown) facedown.insert(id);

        long count[2] = {0, 0};
        for (const auto& [id, o] : s.objects) {
            const int n = listed.count(id) ? listed[id] : 0;
            if (n > 1) sh_.sink.report("zone", "twice " + o.name, where(s), o.name + " is in " + std::to_string(n) + " zone lists");
            const bool listed_zone = o.zone == ZoneType::Hand || o.zone == ZoneType::MainDeck ||
                                     o.zone == ZoneType::RuneDeck || o.zone == ZoneType::Trash ||
                                     o.zone == ZoneType::ChampionZone || o.zone == ZoneType::LegendZone ||
                                     (o.zone == ZoneType::Banishment && !o.is_token);
            if (listed_zone && n == 0)
                sh_.sink.report("zone", "unlisted " + o.name, where(s),
                                o.name + " has zone " + std::to_string(static_cast<int>(o.zone)) + " but is in no list");
            if (o.is_token && (o.zone == ZoneType::Hand || o.zone == ZoneType::MainDeck || o.zone == ZoneType::RuneDeck ||
                               o.zone == ZoneType::Trash || o.zone == ZoneType::ChampionZone || o.zone == ZoneType::LegendZone ||
                               o.zone == ZoneType::FacedownZone || o.zone == ZoneType::Chain))
                sh_.sink.report("token", "zone " + o.name, where(s),
                                "token " + o.name + " in zone " + std::to_string(static_cast<int>(o.zone)));
            if ((o.zone == ZoneType::Base || o.zone == ZoneType::BattlefieldZone) && !o.location &&
                o.card_type != CardType::Battlefield)
                sh_.sink.report("zone", "boardless " + o.name, where(s), o.name + " is on the board without a location");
            if (o.zone == ZoneType::Chain && !on_chain.count(id))
                sh_.sink.report("zone", "chain " + o.name, where(s), o.name + " has zone Chain but no chain item");
            if (o.zone == ZoneType::FacedownZone && !facedown.count(id))
                sh_.sink.report("zone", "facedown " + o.name, where(s), o.name + " is facedown but at no battlefield");
            // negatives
            if (o.damage_marked < 0 || o.buff_count < 0 || o.empowered < 0)
                sh_.sink.report("negative", o.name, where(s), o.name + " has negative damage, buffs or empowered");
            if (!o.is_token && (o.owner == PlayerId::Player1 || o.owner == PlayerId::Player2))
                ++count[static_cast<int>(o.owner) - 1];
        }
        for (int p = 0; p < 2; ++p) {
            if (base_count_[p] < 0) base_count_[p] = count[p];
            else if (count[p] != base_count_[p])
                sh_.sink.report("conserve", "count", where(s),
                                "player " + std::to_string(p + 1) + " owns " + std::to_string(count[p]) +
                                    " non-token objects, " + std::to_string(base_count_[p]) + " before");
            base_count_[p] = count[p];
        }
        // Both players' units at one battlefield at a clean main-phase point.
        if (isCleanResumePoint(s)) {
            for (const auto& bf : s.battlefields) {
                if (bf.hasUnitsFrom(PlayerId::Player1, s.objects) && bf.hasUnitsFrom(PlayerId::Player2, s.objects))
                    sh_.sink.report("bothunits", "bf", where(s), "battlefield " + std::to_string(bf.id) + " holds both players' units");
            }
        }
    }

    int victoryScore(const GameState& s) const {
        int v = s.mode.victory_score;
        for (const auto& bf : s.battlefields) {
            if (!s.objectExists(bf.card_object_id)) continue;
            if (const Card* c = sh_.reg.get(s.getObject(bf.card_object_id).card_def_id)) v += c->victoryScoreModifier();
        }
        return v;
    }

    void checkScores(const GameState& s) {
        const int victory = victoryScore(s);
        for (int p = 0; p < 2; ++p) {
            const auto& ps = s.players[p];
            bool neg = ps.score < 0 || ps.xp < 0 || ps.rune_pool.energy < 0 || ps.rune_pool.universal_power < 0;
            for (int d = 0; d < static_cast<int>(Domain::Count); ++d) neg = neg || ps.rune_pool.power[d] < 0;
            if (neg) sh_.sink.report("negative", "player", where(s), "player " + std::to_string(p + 1) + " has a negative score, XP, energy or power");
            if (ps.score != known_score_[p])
                sh_.sink.report("score", "unexplained", where(s),
                                "player " + std::to_string(p + 1) + " score " + std::to_string(ps.score) +
                                    ", last scoring event said " + std::to_string(known_score_[p]));
            known_score_[p] = ps.score;
            if (ps.score >= victory && ps.score > s.players[1 - p].score)
                sh_.sink.report("score", "past victory", where(s), "a decision is asked with a player at the victory score");
        }
        // Units that entered ready / gear that entered exhausted since the last decision.
        for (auto id : entered_) {
            if (!s.objectExists(id)) continue;
            const auto& o = s.getObject(id);
            if (!o.location) continue;
            std::lock_guard<std::mutex> lock(sh_.misc_mu);
            if (o.isUnit() && !o.is_exhausted) ++sh_.entered_ready_units[o.name];
            if (o.isGear() && o.is_exhausted && !o.attached_to) ++sh_.entered_exhausted_gear[o.name];
        }
        entered_.clear();
    }

    void checkPoolsEmpty(const GameState& s) {
        for (int p = 0; p < 2; ++p) {
            const auto& pool = s.players[p].rune_pool;
            int total = pool.energy + pool.universal_power;
            for (int d = 0; d < static_cast<int>(Domain::Count); ++d) total += pool.power[d];
            if (total != 0)
                sh_.sink.report("pool", "main start", where(s),
                                "player " + std::to_string(p + 1) + " starts the main phase with " + std::to_string(total) +
                                    " in the rune pool");
        }
    }

    void trackReadies(const GameState& s) {
        for (const auto& [id, o] : s.objects) {
            auto it = was_exhausted_.find(id);
            if (it != was_exhausted_.end() && it->second && !o.is_exhausted) ++readies_[id];
            was_exhausted_[id] = o.is_exhausted;
        }
    }

    void checkActivation(const GameState& s, const Intent& in) {
        if (!isActivation(in.type) || in.ability_source == kInvalidId || !s.objectExists(in.ability_source)) return;
        const auto& o = s.getObject(in.ability_source);
        // An aura-granted ability (Forge, Gardens, Heimerdinger) is the granting card's ability.
        const bool granted = in.granted_ability_def != 0;
        const CardDefId def_id = granted ? in.granted_ability_def : o.card_def_id;
        const Card* card = sh_.reg.get(def_id);
        if (!card) return;
        const auto abilities = card->activatedAbilities();
        if (in.ability_index < 0 || in.ability_index >= static_cast<int>(abilities.size())) return;  // equip etc.
        const auto& ab = abilities[in.ability_index];
        const std::string key = o.name + " #" + std::to_string(in.ability_index) +
                                (granted ? " granted by " + sh_.db.get(def_id).name : "");
        if (ab.cost.exhaust && o.is_exhausted)
            sh_.sink.report("exhaust", key, where(s), o.name + " activates an exhaust-cost ability while exhausted");
        const long used = ++uses_[{in.ability_source, in.ability_index + (granted ? 1000 * static_cast<int>(def_id) : 0)}];
        const long readied = readies_.count(in.ability_source) ? readies_[in.ability_source] : 0;
        if (ab.cost.exhaust && used > 1 + readied)
            sh_.sink.report("twice", key, where(s),
                            o.name + " used its exhaust-cost ability " + std::to_string(used) + " times this turn, readied " +
                                std::to_string(readied));
        const std::string text = lower(sh_.db.get(def_id).ability_text);
        if ((text.find("once per turn") != std::string::npos || text.find("use only once") != std::string::npos) &&
            used > 1)
        {
            std::string counters;
            for (const auto& [k, v] : o.card_counters) counters += " " + k + "=" + std::to_string(v);
            for (const auto& [oid, other] : s.objects)
                if (other.card_def_id == o.card_def_id && oid != in.ability_source)
                    counters += " | same card: object " + std::to_string(oid) + " zone " +
                                std::to_string(static_cast<int>(other.zone)) + " at " + locStr(other.location) +
                                " ctl " + std::to_string(static_cast<int>(other.controller));
            sh_.sink.report("twice", key, where(s), o.name + " (object " + std::to_string(in.ability_source) +
                            ", counters" + counters + ") used a once-per-turn ability " + std::to_string(used) +
                            " times this turn");
        }
    }

    void noteUsage(const GameState& s, const Intent& in) {
        if (!isActivation(in.type) || in.ability_source == kInvalidId || !s.objectExists(in.ability_source)) return;
        const CardDefId d = s.getObject(in.ability_source).card_def_id;
        ++turn_acts_[d];
        played_[static_cast<int>(in.player) - 1].insert(d);
    }

    void flushTurnUsage() {
        if (turn_acts_.empty() && turn_triggers_.empty() && turn_plays_.empty()) return;
        std::lock_guard<std::mutex> lock(sh_.usage.mu);
        for (const auto& [d, n] : turn_acts_) {
            auto& u = sh_.usage.cards[d];
            u.activations += n;
            u.max_activations_turn = std::max(u.max_activations_turn, n);
        }
        for (const auto& [d, n] : turn_triggers_) {
            auto& u = sh_.usage.cards[d];
            u.triggers += n;
            u.max_triggers_turn = std::max(u.max_triggers_turn, n);
        }
        for (const auto& [d, n] : turn_plays_) {
            auto& u = sh_.usage.cards[d];
            u.plays += n;
            u.max_plays_turn = std::max(u.max_plays_turn, n);
        }
        turn_acts_.clear();
        turn_triggers_.clear();
        turn_plays_.clear();
    }

    // Every legal intent is applied on a copy: none may throw, and none may leave the game
    // exactly as it was with the same choices offered -- a legal action that changes nothing
    // can be repeated without end (the Sett, Brawler and Hextech Gauntlets loops).
    void tryAllLegal(const GameState& s, const StepResult& step) {
        const std::string before = dumpState(s, false);
        for (size_t i = 0; i < step.legal.size(); ++i) {
            std::vector<std::string> log;   // outlives the bus and engine that write to it
            EventBus b;
            GameEngine e(sh_.db, b, sh_.reg);
            b.on_log.connect([&log](const LogEvent& ev) { log.push_back(ev.message); });
            try {
                StepResult cs = e.resumeFromSnapshot(s, 12345);
                log.clear();
                if (i < cs.legal.size()) {
                    StepResult after = e.applyChoice(static_cast<int>(i));
                    if (after.kind == StepKind::NeedDecision && after.legal == step.legal &&
                        dumpState(e.state(), false) == before) {
                        const auto& in = step.legal[i];
                        std::string key = in.describe().substr(0, 24);
                        if (in.ability_source != kInvalidId && s.objectExists(in.ability_source))
                            key = s.getObject(in.ability_source).name;
                        else if (in.card != kInvalidId && s.objectExists(in.card))
                            key = s.getObject(in.card).name;
                        std::string what = "legal intent changes nothing: " + in.describe();
                        for (const auto& m : log) what += std::string(1, '\n') + "      | " + m;
                        sh_.sink.report("noop", key, where(s), what);
                    }
                }
            } catch (const std::exception& ex) {
                sh_.sink.report("throw", step.legal[i].describe().substr(0, 40), where(s),
                                "legal intent " + step.legal[i].describe() + " threw on a copy: " + ex.what());
            }
            ++sh_.legal_checked;
        }
    }

    void checkEnd(const GameState& s) {
        std::string ending;
        if (!s.game_over) {
            sh_.sink.report("end", "not over", where(s), "the step machine stopped but the game is not over");
            ending = "not over";
        } else if (s.winner == PlayerId::None) {
            ending = "draw: " + s.game_over_reason;
            if (s.game_over_reason.find("Max turns") != std::string::npos)
                sh_.sink.report("turncap", "turns", where(s), "the game hit the turn limit");
        } else {
            const auto& w = s.player(s.winner);
            const auto& l = s.player(s.winner == PlayerId::Player1 ? PlayerId::Player2 : PlayerId::Player1);
            if (w.score >= victoryScore(s)) ending = "victory score";
            else if (l.burned_out) ending = "burn out";
            else {
                ending = "other: " + s.game_over_reason;
                sh_.sink.report("end", "winner below victory", where(s),
                                "winner has " + std::to_string(w.score) + " points: " + s.game_over_reason);
            }
            if (l.score >= w.score)
                sh_.sink.report("end", "loser at victory", where(s), "the loser also reached the victory score");
        }
        std::lock_guard<std::mutex> lock(sh_.misc_mu);
        ++sh_.endings[ending];
    }

    void noteResult(const GameState& s) {
        std::lock_guard<std::mutex> lock(sh_.usage.mu);
        const size_t deck_of[2] = {su_.d1, su_.d2};
        for (int p = 0; p < 2; ++p) {
            const bool won = s.winner == (p == 0 ? PlayerId::Player1 : PlayerId::Player2);
            auto& wl = sh_.usage.deck_wl[sh_.decks.names[deck_of[p]]];
            ++wl.first;
            if (won) ++wl.second;
            for (auto d : played_[p]) {
                auto& u = sh_.usage.cards[d];
                ++u.games_played;
                if (won) ++u.wins_when_played;
                deck_games_[d].push_back(sh_.decks.names[deck_of[p]]);
            }
        }
        // the deck baseline is added at the end (needs all games); keep the deck names
        std::lock_guard<std::mutex> lock2(sh_.misc_mu);
        for (auto& [d, v] : deck_games_)
            for (auto& name : v) played_in_deck_[d][name]++;
    }

public:
    static std::map<CardDefId, std::map<std::string, long>> played_in_deck_;

private:
    void replay() {
        EventBus bus;
        GameEngine engine(sh_.db, bus, sh_.reg);
        StepResult step = engine.beginGame(sh_.decks.decks[su_.d1], sh_.decks.decks[su_.d2], su_.game_seed);
        for (size_t k = 0; k < chosen_.size(); ++k) {
            if (step.kind != StepKind::NeedDecision) {
                sh_.sink.report("determinism", "ended early", where(engine.state()), "replay ended at decision " + std::to_string(k));
                return;
            }
            const std::string d = dumpState(engine.state());
            if (d != trace_[k] || step.legal.size() != legal_sizes_[k]) {
                decision_ = static_cast<long>(k);
                sh_.sink.report("determinism", "state", where(engine.state()), firstDiff(trace_[k], d));
                return;
            }
            step = engine.applyChoice(chosen_[k]);
        }
        ++sh_.replays;
    }

    Shared& sh_;
    GameSetup su_;
    GameEngine* engine_ = nullptr;
    long decision_ = 0;
    long base_count_[2] = {-1, -1};
    int known_score_[2] = {0, 0};
    int random_events_ = 0;
    std::vector<GameObjectId> entered_;
    std::map<std::pair<GameObjectId, int>, long> uses_;
    std::map<GameObjectId, long> readies_;
    std::unordered_map<GameObjectId, bool> was_exhausted_;
    std::map<CardDefId, long> turn_acts_, turn_triggers_, turn_plays_;
    std::set<CardDefId> played_[2];
    std::map<CardDefId, std::vector<std::string>> deck_games_;
    std::vector<int> chosen_;
    std::vector<std::string> trace_;
    std::vector<size_t> legal_sizes_;
};

std::map<CardDefId, std::map<std::string, long>> GameChecker::played_in_deck_;

const char* typeName(CardType t) {
    switch (t) {
        case CardType::Unit: return "unit";
        case CardType::Gear: return "gear";
        case CardType::Spell: return "spell";
        case CardType::Rune: return "rune";
        case CardType::Legend: return "legend";
        case CardType::Battlefield: return "battlefield";
        default: return "other";
    }
}

std::string csvQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

void writeScope(const Options& opt, const CardDB& db, const tools::DeckSet& decks) {
    std::map<CardDefId, std::set<size_t>> in_decks;
    for (size_t i = 0; i < decks.decks.size(); ++i) {
        const auto& d = decks.decks[i];
        in_decks[d.legend].insert(i);
        in_decks[d.chosen_champion].insert(i);
        for (auto c : d.main_deck) in_decks[c].insert(i);
        for (auto c : d.rune_deck) in_decks[c].insert(i);
        for (auto c : d.battlefields) in_decks[c].insert(i);
    }
    std::ofstream out(opt.scope_out);
    out << "id,name,type,decks\n";
    for (const auto& [id, set] : in_decks) {
        if (id == 0) continue;
        const auto& def = db.get(id);
        out << id << ',' << csvQuote(def.name) << ',' << typeName(def.card_type) << ',' << set.size() << "\n";
    }
    std::cerr << in_decks.size() << " cards in " << decks.decks.size() << " decks -> " << opt.scope_out << "\n";
}

void writeUsage(const Options& opt, Shared& sh) {
    std::ofstream out(opt.usage_out);
    out << "id,name,games_played,plays_per_game,max_plays_turn,activations_per_game,max_activations_turn,"
           "triggers_per_game,max_triggers_turn,win_rate_when_played,deck_win_rate,win_rate_delta\n";
    for (const auto& [d, u] : sh.usage.cards) {
        if (u.games_played == 0) continue;
        double expected = 0;
        long n = 0;
        for (const auto& [deck, k] : GameChecker::played_in_deck_[d]) {
            const auto& wl = sh.usage.deck_wl[deck];
            if (wl.first > 0) { expected += k * static_cast<double>(wl.second) / wl.first; n += k; }
        }
        const double wr = static_cast<double>(u.wins_when_played) / u.games_played;
        const double base = n > 0 ? expected / n : 0.5;
        const double g = static_cast<double>(u.games_played);
        out << d << ',' << csvQuote(sh.db.all().count(d) ? sh.db.get(d).name : "#" + std::to_string(d)) << ',' << u.games_played << ',' << u.plays / g << ','
            << u.max_plays_turn << ',' << u.activations / g << ',' << u.max_activations_turn << ',' << u.triggers / g << ','
            << u.max_triggers_turn << ',' << wr << ',' << base << ',' << wr - base << "\n";
    }
    std::cerr << "usage -> " << opt.usage_out << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << a << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--games") opt.games = std::stoi(next());
        else if (a == "--threads") opt.threads = std::stoi(next());
        else if (a == "--seed") opt.seed = std::stoull(next());
        else if (a == "--decks") opt.decks_dir = next();
        else if (a == "--agents") opt.agents = splitSemis(next());
        else if (a == "--determinism-every") opt.determinism_every = std::stoi(next());
        else if (a == "--copy-every") opt.copy_every = std::stoi(next());
        else if (a == "--all-legal-every") opt.all_legal_every = std::stoi(next());
        else if (a == "--only-game") opt.only_game = std::stoi(next());
        else if (a == "--max-print") opt.max_print = std::stoi(next());
        else if (a == "--trace") opt.trace = true;
        else if (a == "--scope") opt.scope_out = next();
        else if (a == "--usage") opt.usage_out = next();
        else {
            std::cerr << "usage: riftbound_rules_fuzz [--games N] [--threads T] [--seed S] [--decks DIR] "
                         "[--agents SPEC;SPEC] [--determinism-every K] [--copy-every K] [--all-legal-every K] "
                         "[--only-game G] [--max-print N] [--scope FILE] [--usage FILE]\n";
            return 2;
        }
    }

    CardRegistry reg;
    reg.loadAll();
    CardDB db;
    db.buildFromClasses(reg);
    const auto decks = tools::loadLegalDecks(opt.decks_dir, db);
    if (decks.decks.size() < 1) { std::cerr << "no legal decks in " << opt.decks_dir << "\n"; return 1; }
    if (!opt.scope_out.empty()) { writeScope(opt, db, decks); return 0; }

    Shared sh(opt, db, reg, decks);
    sh.sink.max_print = opt.max_print;
    try {
        for (const auto& a : opt.agents) sh.specs.push_back(tools::parseToolAgent(a));
    } catch (const std::exception& e) {
        std::cerr << "bad agent: " << e.what() << "\n";
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    std::atomic<int> next_game{opt.only_game >= 0 ? opt.only_game : 0};
    const int end_game = opt.only_game >= 0 ? opt.only_game + 1 : opt.games;
    std::atomic<int> done{0};
    const auto worker = [&] {
        for (int g = next_game++; g < end_game; g = next_game++) {
            const GameSetup su = setupFor(opt, sh, g);
            GameChecker checker(sh, su);
            try {
                checker.run();
            } catch (const std::exception& e) {
                sh.sink.report("throw", "checker", "game " + std::to_string(g), e.what());
            }
            const int n = ++done;
            if (n % 500 == 0) {
                std::lock_guard<std::mutex> lock(sh.sink.mu);
                std::cerr << n << " games, " << sh.decisions.load() << " decisions\n";
            }
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < std::max(1, opt.threads); ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "\n== summary: " << done.load() << " games, " << sh.decisions.load() << " decisions, "
              << sh.copies.load() << " copy checks, " << sh.legal_checked.load() << " legal intents applied on copies, "
              << sh.replays.load() << " games replayed, " << secs << " s\n";
    std::cout << "violations by check:";
    if (sh.sink.by_check.empty()) std::cout << " none";
    for (const auto& [c, n] : sh.sink.by_check) std::cout << ' ' << c << '=' << n;
    std::cout << "\nviolations by key:\n";
    for (const auto& [k, n] : sh.sink.counts) std::cout << "  " << n << "  " << k.first << " [" << k.second << "]\n";
    std::cout << "endings:\n";
    for (const auto& [e, n] : sh.endings) std::cout << "  " << n << "  " << e << "\n";
    const auto top = [](const std::map<std::string, long>& m, const char* title) {
        std::vector<std::pair<long, std::string>> v;
        for (const auto& [k, n] : m) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        std::cout << title << " (for triage, not violations):\n";
        for (size_t i = 0; i < v.size() && i < 40; ++i) std::cout << "  " << v[i].first << "  " << v[i].second << "\n";
    };
    top(sh.entered_ready_units, "units seen ready right after entering the board");
    top(sh.entered_exhausted_gear, "gear seen exhausted right after entering the board");
    if (!opt.usage_out.empty()) writeUsage(opt, sh);
    return sh.sink.by_check.empty() ? 0 : 3;
}
