#include "replay_writer.h"

#include "core/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace riftbound {

std::map<std::string, std::string> ReplayWriter::loadCardImageMap() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates = {
        fs::path("cards/raw/gallery_raw.json"),
    };
    if (const char* root = std::getenv("RIFTBOUND_ROOT")) {
        candidates.push_back(fs::path(root) / "cards" / "raw" / "gallery_raw.json");
    }
    // Walk up from CWD as a fallback (devs often run from build/).
    for (fs::path p = fs::current_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        candidates.push_back(p / "cards" / "raw" / "gallery_raw.json");
    }

    std::map<std::string, std::string> out;
    for (const auto& path : candidates) {
        std::ifstream in(path);
        if (!in) continue;
        nlohmann::json arr;
        try { in >> arr; } catch (...) { continue; }
        if (!arr.is_array()) continue;
        for (const auto& card : arr) {
            if (!card.contains("name") || !card.contains("cardImage")) continue;
            const auto& img = card["cardImage"];
            if (!img.contains("url")) continue;
            std::string name = card["name"].get<std::string>();
            std::string url  = img["url"].get<std::string>();
            // First-write-wins so duplicate names across sets keep the
            // earliest entry. The image is the same illustration anyway
            // for shared names.
            out.emplace(std::move(name), std::move(url));
        }
        if (!out.empty()) return out;
    }
    return out;  // empty if no gallery file found
}

namespace {

int seatOf(PlayerId p) {
    if (p == PlayerId::Player1) return 0;
    if (p == PlayerId::Player2) return 1;
    return -1;
}

/// Keywords worth a badge on a card in play, with their values.
std::string boardKeywords(const GameObject& obj) {
    std::string out;
    auto add = [&](Keyword kw, const char* label, int value = 0) {
        if (!obj.keywords.has(kw)) return;
        if (!out.empty()) out += ' ';
        out += label;
        if (value > 1) out += std::to_string(value);
    };
    add(Keyword::Assault, "Assault", obj.assault_value);
    add(Keyword::Shield, "Shield", obj.shield_value);
    add(Keyword::Deflect, "Deflect", obj.deflect_value);
    add(Keyword::Tank, "Tank");
    add(Keyword::Backline, "Backline");
    add(Keyword::Ganking, "Ganking");
    add(Keyword::Accelerate, "Accelerate");
    add(Keyword::Deathknell, "Deathknell");
    add(Keyword::Temporary, "Temporary");
    add(Keyword::Hidden, "Hidden");
    return out;
}

} // namespace

std::string ReplayWriter::boardJson(const GameState& state, const Intent* chosen) {
    using nlohmann::json;
    std::set<GameObjectId> acted;
    if (chosen) {
        auto mark = [&](GameObjectId id) { if (id != kInvalidId) acted.insert(id); };
        mark(chosen->card);
        mark(chosen->ability_source);
        for (auto id : chosen->targets) mark(id);
        for (auto id : chosen->units_to_move) mark(id);
        for (auto id : chosen->chosen_objects) mark(id);
        for (auto id : chosen->cards_to_mulligan) mark(id);
    }

    // c = card definition (its name and image are in CARDS), n = a name
    // when there is no definition or it differs, x = exhausted, m / bm =
    // might now / printed, d = damage, b = buffs, k = keywords, cd =
    // attacker / defender, e = attached gear, h = facedown, s = acted on.
    auto obj = [&](GameObjectId id) -> json {
        json j = json::object();
        if (!state.objectExists(id)) return j;
        const auto& o = state.getObject(id);
        j["id"] = id;
        if (o.card_def_id != kInvalidId) {
            used_defs_.insert(o.card_def_id);
            j["c"] = o.card_def_id;
            if (o.name != db_.get(o.card_def_id).name) j["n"] = o.name;
        } else {
            j["n"] = o.name.empty() ? std::string("Token") : o.name;
        }
        if (o.is_exhausted) j["x"] = 1;
        if (o.isUnit()) {
            j["m"] = o.current_might;
            j["bm"] = o.base_might;
            if (o.damage_marked > 0) j["d"] = o.damage_marked;
        }
        if (o.buff_count + o.temp_buff_count > 0) j["b"] = o.buff_count + o.temp_buff_count;
        if (o.is_token) j["tok"] = 1;
        if (o.empowered > 0) j["emp"] = o.empowered;
        if (o.is_hidden) j["h"] = 1;
        if (o.is_hidden || o.zone == ZoneType::FacedownZone) j["ctrl"] = seatOf(o.controller);
        auto kws = boardKeywords(o);
        if (!kws.empty()) j["k"] = kws;
        if (o.combat_designation == CombatDesignation::Attacker) j["cd"] = "A";
        if (o.combat_designation == CombatDesignation::Defender) j["cd"] = "D";
        if (acted.count(id)) j["s"] = 1;
        if (!o.attachments.empty()) {
            json e = json::array();
            for (auto a : o.attachments) {
                if (!state.objectExists(a)) continue;
                const auto& g = state.getObject(a);
                json gj = {{"id", a}, {"n", g.name}};
                if (g.card_def_id != kInvalidId) {
                    used_defs_.insert(g.card_def_id);
                    gj["c"] = g.card_def_id;
                }
                e.push_back(std::move(gj));
            }
            j["e"] = std::move(e);
        }
        return j;
    };
    auto list = [&](const std::vector<GameObjectId>& ids) {
        json a = json::array();
        for (auto id : ids) if (state.objectExists(id)) a.push_back(obj(id));
        return a;
    };

    json players = json::array();
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        const auto& ps = state.player(pid);
        std::vector<GameObjectId> units, gear, runes;
        for (const auto& [id, o] : state.objects) {
            if (!o.location || *o.location != LocationId{BaseLocation{pid}}) continue;
            if (o.isUnit()) units.push_back(id);
            else if (o.isGear() && !o.attached_to) gear.push_back(id);
            else if (o.isRune()) runes.push_back(id);
        }
        for (auto* v : {&units, &gear, &runes}) std::sort(v->begin(), v->end());
        // Restricted floating resources, e.g. "+1 P gear only".
        std::string restricted;
        {
            static const char* kScope[] = {"units", "spells", "gear", "units/gear", "showdowns"};
            for (int i = 0; i < RunePool::kScopes; ++i) {
                const auto& r = ps.rune_pool.restricted[i];
                if (r.energy > 0)
                    restricted += " +" + std::to_string(r.energy) + " E " + kScope[i] + " only";
                if (r.power > 0)
                    restricted += " +" + std::to_string(r.power) + " P " + kScope[i] + " only";
            }
        }
        json p = {
            {"score", ps.score},
            {"deck", ps.main_deck.size()},
            {"rdeck", ps.rune_deck.size()},
            {"energy", ps.rune_pool.energy},
            {"power", ps.rune_pool.totalPower()},
            {"restricted_energy", ps.rune_pool.restrictedEnergyTotal()},
            {"restricted_power", ps.rune_pool.restrictedPowerTotal()},
            {"restricted", restricted},
            {"hand", list(ps.hand)},
            {"units", list(units)},
            {"gear", list(gear)},
            {"runes", list(runes)},
            {"trash", list(ps.trash)},
            {"ban", list(ps.banishment)},
        };
        if (state.objectExists(ps.legend_zone)) p["legend"] = obj(ps.legend_zone);
        if (ps.champion_zone != kInvalidId && state.objectExists(ps.champion_zone) &&
            state.getObject(ps.champion_zone).zone == ZoneType::ChampionZone) {
            p["champ"] = obj(ps.champion_zone);
        }
        players.push_back(std::move(p));
    }

    json bfs = json::array();
    for (const auto& bf : state.battlefields) {
        std::string flags;
        auto flag = [&](bool on, const char* f) {
            if (!on) return;
            if (!flags.empty()) flags += " \xC2\xB7 ";
            flags += f;
        };
        flag(bf.is_contested, "Contested");
        flag(bf.combat_in_progress, "Combat");
        flag(bf.showdown_in_progress, "Showdown");
        flag(bf.combat_staged, "Combat staged");
        flag(bf.showdown_staged, "Showdown staged");
        bfs.push_back(json{
            {"card", obj(bf.card_object_id)},
            {"name", StateRenderer::battlefieldLabel(state, bf.id)},
            {"ctrl", bf.controller ? seatOf(*bf.controller) : -1},
            {"flags", flags},
            {"p1", list(state.unitsAt(BattlefieldLocation{bf.id}, PlayerId::Player1))},
            {"p2", list(state.unitsAt(BattlefieldLocation{bf.id}, PlayerId::Player2))},
            {"fd", list(bf.facedown)},
        });
    }

    json chain = json::array();
    auto chainItem = [&](const ChainItem& item, bool resolving) {
        json c = obj(item.source);
        if (!c.contains("c") && item.card_def_id != kInvalidId) {
            used_defs_.insert(item.card_def_id);
            c["c"] = item.card_def_id;
        }
        c["ctrl"] = seatOf(item.controller);
        if (item.is_ability) c["ab"] = 1;
        if (resolving) c["r"] = 1;
        chain.push_back(std::move(c));
    };
    if (state.chain.resuming) chainItem(*state.chain.resuming, true);
    for (int i = static_cast<int>(state.chain.items.size()) - 1; i >= 0; --i) {
        chainItem(state.chain.items[i], false);
    }

    json board = {
        {"turn", state.turn.turn_number},
        {"tp", seatOf(state.turn.turn_player)},
        {"phase", toString(state.turn.phase)},
        {"showdown", state.turn.ns_state == NeutralShowdownState::Showdown},
        {"players", std::move(players)},
        {"bfs", std::move(bfs)},
        {"chain", std::move(chain)},
    };
    if (state.game_over) board["over"] = state.game_over_reason;
    return board.dump();
}

ReplayFormat parseReplayFormat(const std::string& name) {
    if (name == "cards") return ReplayFormat::Cards;
    if (name == "text")  return ReplayFormat::Text;
    throw std::invalid_argument("replay format must be 'cards' or 'text', got '" +
                                name + "'");
}

const char* replayExtension(ReplayFormat format) {
    return format == ReplayFormat::Text ? "txt" : "html";
}

ReplayWriter::ReplayWriter(const CardDB& db, const std::string& output_path,
                           ReplayFormat format)
    : db_(db), output_path_(output_path), format_(format) {}

void ReplayWriter::write() {
    if (format_ == ReplayFormat::Text) writeText();
    else writeHtml();
}

void ReplayWriter::writeText() {
    std::ofstream file(output_path_);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open replay file: " + output_path_);
    }
    auto endLine = [&file](const std::string& s) {
        if (!s.empty() && s.back() != '\n') file << '\n';
    };
    file << "Riftbound replay | " << kVersionTag << '\n';
    if (!game_header_.empty()) file << game_header_ << '\n';
    file << snapshots_.size() << " snapshots (a board is printed only when it "
            "differs from the last one printed)\n";
    // The board without its "| TURN ... | D:<n> ..." header line, which
    // changes at every decision even when nothing on the board does.
    auto boardBody = [](const std::string& board) {
        const size_t turn = board.find("| TURN ");
        if (turn == std::string::npos) return board;
        const size_t end = board.find('\n', turn);
        return board.substr(0, turn) +
               (end == std::string::npos ? std::string() : board.substr(end + 1));
    };
    std::string last_board;
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        const auto& snap = snapshots_[i];
        file << "\n==== [" << (i + 1) << "/" << snapshots_.size() << "] "
             << snap.turn_info << " ====\n";
        if (snap.mcts_player >= 0) {
            const double p = (snap.mcts_value + 1.0) / 2.0;
            file << "Win estimate: P" << (snap.mcts_player + 1) << " "
                 << static_cast<int>(p * 100.0 + 0.5) << "% (" << snap.mcts_sims
                 << " sims)\n";
        }
        if (!snap.board_ascii.empty()) {
            std::string body = boardBody(snap.board_ascii);
            if (body == last_board) {
                file << "(board unchanged)\n";
            } else {
                // Drop each line's box padding ("text      |" -> "text").
                std::istringstream lines(snap.board_ascii);
                for (std::string line; std::getline(lines, line);) {
                    size_t end = line.size();
                    if (end > 0 && line[end - 1] == '|') --end;
                    while (end > 0 && line[end - 1] == ' ') --end;
                    file.write(line.data(), static_cast<std::streamsize>(end));
                    file << '\n';
                }
                last_board = std::move(body);
            }
        }
        if (!snap.decision_text.empty()) {
            file << "-- Decision --\n" << snap.decision_text;
            endLine(snap.decision_text);
        }
        if (!snap.trace_lines.empty()) {
            file << "-- Log --\n";
            for (const auto& line : snap.trace_lines) file << line << '\n';
        }
    }
}

void ReplayWriter::setGameHeader(const std::string& header) {
    game_header_ = header;
}

void ReplayWriter::setNextWinProb(double mean_value, int sims, int deciding_player) {
    pending_mcts_value_  = mean_value;
    pending_mcts_sims_   = sims;
    pending_mcts_player_ = deciding_player;
}

void ReplayWriter::addTraceLine(const std::string& line) {
    pending_trace_.push_back(line);
}

void ReplayWriter::recordPhaseSeam(const GameState& state,
                                    TurnPhase old_phase,
                                    TurnPhase new_phase,
                                    const StateRenderer& renderer) {
    // Skip until the first decision has been recorded — pre-game phase
    // transitions (Setup → AwakenPhase during engine startup) happen
    // before any user-visible decision and would create orphan seams.
    if (snapshots_.empty()) return;
    // Ignore no-op transitions (engine re-emits the current phase in
    // some paths — e.g. resetting AwakenPhase mid-startup).
    if (old_phase == new_phase) return;
    // Suppress back-to-back duplicate seams: if the most recent
    // snapshot is already a seam for this same phase, no new chapter
    // marker is needed. We STILL flush pending_trace_ to it — otherwise
    // phase-header trace lines (logged inside awakenPhase / beginningStep
    // / etc. just before they emit their PhaseChangedEvent, which the
    // duplicate-emit path re-fires) would leak forward onto the NEXT
    // seam, mis-labeling them.
    if (snapshots_.back().is_seam &&
        snapshots_.back().seam_phase == toString(new_phase)) {
        if (!pending_trace_.empty()) {
            auto& prev = snapshots_.back();
            prev.trace_lines.insert(
                prev.trace_lines.end(),
                std::make_move_iterator(pending_trace_.begin()),
                std::make_move_iterator(pending_trace_.end()));
            pending_trace_.clear();
        }
        return;
    }

    // Flush whatever trace has accumulated since the last snapshot to
    // the most recent snapshot — those events were caused by the decision
    // (or seam) that came before this transition. Each phase's trace
    // thus localizes to the snapshot that owns that phase.
    if (!pending_trace_.empty()) {
        auto& prev = snapshots_.back();
        prev.trace_lines.insert(
            prev.trace_lines.end(),
            std::make_move_iterator(pending_trace_.begin()),
            std::make_move_iterator(pending_trace_.end()));
        pending_trace_.clear();
    }

    Snapshot snap;
    snap.is_seam = true;
    snap.seam_phase = toString(new_phase);
    snap.decision_index = state.decision_index;

    std::ostringstream ti;
    ti << "Turn " << state.turn.turn_number
       << " | ▶ " << snap.seam_phase << " begins"
       << " | " << toString(state.turn.turn_player) << "'s turn"
       << " | Score: P1=" << state.players[0].score
       << " P2=" << state.players[1].score;
    snap.turn_info = ti.str();

    if (new_phase != TurnPhase::Mulligan) snap.board_ascii = renderer.render(state);
    if (format_ == ReplayFormat::Cards) snap.board_json = boardJson(state, nullptr);

    std::ostringstream dec;
    dec << "  ── Phase transition ──\n";
    dec << "  " << toString(old_phase) << "  →  " << snap.seam_phase << "\n";
    snap.decision_text = dec.str();

    snapshots_.push_back(std::move(snap));
}

void ReplayWriter::recordDecision(const GameState& state,
                                   const std::vector<Intent>& legal_actions,
                                   const Intent& chosen_action,
                                   const StateRenderer& renderer) {
    // Forward-shift the trace: events accumulated since the previous
    // recordDecision were caused by applying the previous decision's
    // chosen_action (plus any intervening system steps). Attach them to
    // the PREVIOUS snapshot so each decision's trace shows ITS
    // consequences, not the prior decision's. For the very first call
    // there's no previous snapshot — pre-game/mulligan-setup trace
    // attaches to snapshot 0 below.
    if (!snapshots_.empty() && !pending_trace_.empty()) {
        auto& prev = snapshots_.back();
        prev.trace_lines.insert(
            prev.trace_lines.end(),
            std::make_move_iterator(pending_trace_.begin()),
            std::make_move_iterator(pending_trace_.end()));
        pending_trace_.clear();
    }

    Snapshot snap;
    snap.decision_index = state.decision_index;

    // Turn info
    std::ostringstream ti;
    ti << "Turn " << state.turn.turn_number
       << " | " << toString(state.turn.phase)
       << " | " << toString(state.turn.turn_player) << "'s turn"
       << " | Decision #" << state.decision_index
       << " | Score: P1=" << state.players[0].score
       << " P2=" << state.players[1].score;
    snap.turn_info = ti.str();

    // Board ASCII (only for non-mulligan); the picture board also shows the
    // opening hands while they are mulliganed.
    if (state.turn.phase != TurnPhase::Mulligan) snap.board_ascii = renderer.render(state);
    if (format_ == ReplayFormat::Cards) snap.board_json = boardJson(state, &chosen_action);

    // Decision text, one option per line: the panel wraps them to its width.
    StateRenderer listing = renderer;
    listing.max_width = 0;
    snap.decision_text = listing.renderDecision(state, legal_actions, chosen_action);

    // Pre-game trace (anything emitted before the very first decision —
    // mulligan setup, opening hand, etc.) lives on snapshot 0 since there
    // is no "previous decision" to own it. Once the first snapshot exists,
    // all subsequent trace flushes go to the previous snapshot above.
    if (snapshots_.empty()) {
        snap.trace_lines = std::move(pending_trace_);
        pending_trace_.clear();
    }

    // Consume any pending MCTS value (set by setNextWinProb just before
    // this call). Cleared after consumption so the next decision either
    // gets a fresh value or reads as "no estimate."
    snap.mcts_value  = pending_mcts_value_;
    snap.mcts_sims   = pending_mcts_sims_;
    snap.mcts_player = pending_mcts_player_;
    pending_mcts_value_  = -2.0;
    pending_mcts_sims_   = 0;
    pending_mcts_player_ = -1;

    snapshots_.push_back(std::move(snap));
}

std::string ReplayWriter::escapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string ReplayWriter::escapeJs(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void ReplayWriter::writeHtml() {
    std::ofstream file(output_path_);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open replay file: " + output_path_);
    }

    file << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Riftbound Game Replay</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: #1a1a2e; color: #e0e0e0;
    font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
    font-size: 13px; overflow: hidden; height: 100vh;
    display: flex; flex-direction: column;
}
/* The header and turn bar keep to one line each (the full game header is
   the title's tooltip) so #main gets the rest of the window. */
#header {
    flex: none; background: #16213e; padding: 6px 16px; border-bottom: 2px solid #0f3460;
    display: flex; justify-content: space-between; align-items: center; gap: 16px;
}
#header h1 {
    font-size: 14px; color: #e94560; min-width: 0;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
#nav { flex: none; display: flex; gap: 8px; align-items: center; }
#nav button {
    background: #0f3460; color: #e0e0e0; border: 1px solid #e94560; white-space: nowrap;
    padding: 4px 12px; cursor: pointer; font-family: inherit; font-size: 12px;
}
#nav button:hover { background: #e94560; }
#counter { color: #a0a0a0; font-size: 12px; min-width: 120px; text-align: center; white-space: nowrap; }
#turn-info {
    flex: none; background: #0f3460; padding: 5px 16px; font-size: 13px;
    color: #e94560; font-weight: bold; border-bottom: 1px solid #333;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
/* Phase-seam visual treatment — distinct from decision snapshots so a
   reader scrubbing through with arrow keys can spot phase boundaries
   instantly. Yellow band; the right-panel decision area carries a big
   "▶ PHASE BEGINS" banner. */
body.seam #turn-info {
    background: #5d3a00; color: #ffd166;
    box-shadow: 0 0 8px #ffd16640 inset;
}
body.seam #decision-panel {
    background: #2a1e08;
    border-bottom: 2px solid #ffd166;
}
body.seam #decision-panel pre { color: #ffd166; }
#main { display: flex; flex: 1; min-height: 0; }
#board-panel {
    flex: 3; overflow-y: auto; padding: 8px; border-right: 2px solid #0f3460;
    min-width: 0;
}
#right-panel { flex: 2; display: flex; flex-direction: column; min-width: 0;
    border-right: 2px solid #0f3460; }
#decision-panel {
    padding: 8px; border-bottom: 2px solid #0f3460;
    max-height: 40%; overflow-y: auto;
}
#trace-panel { flex: 1; padding: 8px; overflow-y: auto; }
/* Card-details side pane: shows the official card image for any name
   the user clicked in board / decision / trace panes. Card-link spans
   are added by JS at show() time over every text node. */
#card-details-panel {
    flex: 1.6; padding: 12px; overflow-y: auto;
    display: flex; flex-direction: column; align-items: center;
    background: #0c1933; min-width: 0;
}
#card-details-panel .label { align-self: stretch; }
#card-details-name {
    color: #feca57; font-size: 14px; font-weight: bold;
    margin-bottom: 8px; text-align: center; min-height: 1.4em;
}
#card-details-image {
    flex: 0 1 auto; min-height: 0; max-width: 100%;
    object-fit: contain; border: 1px solid #0f3460;
    background: #16213e;
    display: none;  /* hidden until first click */
}
#card-details-placeholder {
    color: #5a6985; font-size: 12px; text-align: center; margin-top: 40px;
    font-style: italic;
}
.card-link {
    cursor: pointer; color: #54a0ff; text-decoration: underline dotted;
    text-underline-offset: 2px;
}
.card-link:hover { color: #feca57; background: #16213e; }
.card-link.active { color: #feca57; background: #2a1e08; }
/* Drag handles for resizing panes. Vertical handles (col-resize)
   sit between flex children of #main and resize the panel to their
   LEFT by tweaking its flex-basis. Horizontal handles (row-resize)
   resize the panel ABOVE them within #right-panel.
   The handle itself is 4px wide/tall with a 1px accent line; hover
   widens the visual hit area. Drag state lives in JS — no
   localStorage, sizing resets each page load. */
.resize-handle-v {
    flex: 0 0 4px; background: #0f3460; cursor: col-resize;
    transition: background 0.1s;
}
.resize-handle-v:hover, .resize-handle-v.dragging { background: #e94560; }
.resize-handle-h {
    flex: 0 0 4px; background: #0f3460; cursor: row-resize;
    transition: background 0.1s;
}
.resize-handle-h:hover, .resize-handle-h.dragging { background: #e94560; }
body.dragging-pane { cursor: col-resize; user-select: none; }
body.dragging-pane-v { cursor: row-resize; user-select: none; }
pre {
    white-space: pre; font-family: inherit; font-size: 12px; line-height: 1.4;
}
/* Decision and trace text wrap to the panel instead of scrolling sideways. */
#decision-panel pre, #trace-panel pre { white-space: pre-wrap; overflow-wrap: anywhere; }
#board-panel pre { color: #c8d6e5; }
/* Picture view of the board (default; T or the toggle switches to the
   text board). Cards are the official images, thumbnailed by the CDN;
   exhausted cards are turned sideways, the decision's cards outlined. */
body:not(.textview) #board-panel { flex: 5; }
body.textview #vboard { display: none; }
body:not(.textview) #board { display: none; }
.board-label { display: flex; justify-content: space-between; align-items: center; }
#view-toggle button {
    background: #16213e; color: #a0a0a0; border: 1px solid #0f3460;
    padding: 1px 8px; font: inherit; font-size: 10px; cursor: pointer;
    text-transform: uppercase;
}
#view-toggle button.on { background: #0f3460; color: #feca57; border-color: #feca57; }
#vboard {
    display: flex; flex-direction: column; gap: 6px;
    font-family: system-ui, 'Segoe UI', Roboto, sans-serif; font-size: 12px;
}
#vboard .empty-note { color: #5a6985; font-style: italic; padding: 24px; text-align: center; }
.pstrip {
    display: flex; align-items: center; gap: 12px; padding: 6px 10px;
    background: #16213e; border-radius: 6px; flex-wrap: wrap;
}
.pstrip.p1 { border-left: 4px solid #54a0ff; }
.pstrip.p2 { border-left: 4px solid #ff6b6b; }
.pstrip.turn { box-shadow: 0 0 0 1px #feca57 inset, 0 0 10px #feca5730; }
.pname { font-weight: 800; font-size: 15px; }
.pname small { display: block; font-size: 9px; font-weight: 400; color: #feca57; }
.c0 { color: #54a0ff; } .c1 { color: #ff6b6b; }
.score {
    font-size: 26px; font-weight: 800; color: #feca57; text-align: center; line-height: 1;
    min-width: 46px;
}
.score small { display: block; font-size: 9px; color: #888; font-weight: 400; }
.stat { font-size: 11px; color: #a0a0a0; line-height: 1.6; white-space: nowrap; }
.stat b { color: #e0e0e0; }
.zone { display: flex; flex-direction: column; gap: 3px; min-width: 0; }
.zone > .zl {
    font-size: 9px; color: #5a6985; text-transform: uppercase; letter-spacing: .5px;
}
.row { display: flex; flex-wrap: wrap; gap: 4px; align-items: center; min-height: 18px; }
.base {
    display: flex; gap: 18px; padding: 6px 10px; background: #121a33;
    border-radius: 6px; align-items: flex-start; flex-wrap: wrap; min-height: 40px;
}
.base.p1 { border-left: 4px solid #54a0ff55; }
.base.p2 { border-left: 4px solid #ff6b6b55; }
.chainrow {
    display: flex; gap: 8px; align-items: center; padding: 6px 10px;
    background: #2a1e08; border: 1px solid #feca57; border-radius: 6px;
}
.chainrow .zl { color: #feca57; font-size: 10px; text-transform: uppercase; }
.bfs { display: flex; gap: 8px; }
.bf {
    flex: 1; min-width: 0; background: #101830; border: 2px solid #0f3460;
    border-radius: 8px; overflow: hidden; display: flex; flex-direction: column;
}
.bf.ctrl0 { border-color: #54a0ff; }
.bf.ctrl1 { border-color: #ff6b6b; }
.bf.hot { box-shadow: 0 0 12px #f9ca2480; }
.bfhead {
    position: relative; height: calc(64px * var(--k, 1)); background: #22304f center/cover no-repeat;
    cursor: pointer;
}
.bfname {
    position: absolute; left: 0; right: 0; bottom: 0; padding: 3px 8px;
    background: linear-gradient(transparent, rgba(0,0,0,.9));
    font-size: 12px; font-weight: 600; color: #fff;
}
.bfname .ctl { float: right; font-size: 10px; }
.bfflags { color: #f9ca24; font-size: 10px; margin-left: 6px; font-weight: 700; }
.side {
    padding: 6px; min-height: calc(96px * var(--k, 1)); display: flex; flex-wrap: wrap; gap: 6px;
    align-items: center; justify-content: center;
}
.side.s0 { background: rgba(84,160,255,.07); }
.side.s1 { background: rgba(255,107,107,.07); }
.pile { position: relative; cursor: pointer; }
/* A card: .slot holds the badges upright while .art turns when exhausted. */
.slot {
    position: relative; flex: none; width: var(--w); height: calc(var(--w) * 1.4);
    cursor: pointer;
}
.slot.exh { width: calc(var(--w) * 1.4); }
.slot.land { width: calc(var(--w) * 1.4); height: var(--w); }
.art {
    position: absolute; left: 50%; top: 50%; width: var(--w); height: calc(var(--w) * 1.4);
    transform: translate(-50%, -50%); border-radius: calc(var(--w) * .06);
    background: #2d3a5c center/cover no-repeat; box-shadow: 0 1px 4px rgba(0,0,0,.7);
    transition: transform .15s;
}
.slot.land .art { width: calc(var(--w) * 1.4); height: var(--w); }
.slot.exh .art { transform: translate(-50%, -50%) rotate(90deg); filter: brightness(.6); }
.art.txt {
    display: flex; align-items: center; justify-content: center; text-align: center;
    font-size: 9px; padding: 3px; color: #ddd; overflow: hidden;
    border: 1px solid #54a0ff55;
}
.slot:hover .art { outline: 2px solid #feca57; }
.slot.sel .art { outline: 3px solid #feca57; box-shadow: 0 0 14px #feca57; }
.slot.fd .art { filter: brightness(.45) saturate(.4); }
.badge {
    position: absolute; font: 700 11px/1 system-ui, sans-serif; padding: 2px 5px;
    border-radius: 9px; pointer-events: none; z-index: 2; white-space: nowrap;
}
.b-might { right: -5px; bottom: -5px; background: #1e272e; border: 1px solid #c8d6e5; color: #fff; }
.b-might.up { color: #1dd1a1; border-color: #1dd1a1; }
.b-might.down { color: #ff9f43; border-color: #ff9f43; }
.b-dmg { right: -5px; top: -5px; background: #ee5253; color: #fff; }
.b-cd { left: -5px; top: -5px; background: #f9ca24; color: #222; }
.b-count { right: -5px; top: -5px; background: #0f3460; color: #feca57; border: 1px solid #feca57; }
.b-tag { left: -5px; bottom: -5px; background: #5f27cd; color: #fff; font-size: 9px; }
.b-res { left: 50%; top: -7px; transform: translateX(-50%); background: #feca57; color: #222; font-size: 9px; }
.kws {
    position: absolute; left: 2px; right: 2px; top: 34%; display: flex; flex-direction: column;
    gap: 1px; align-items: flex-start; pointer-events: none; z-index: 2;
}
.kws span {
    font: 700 8px/1 system-ui, sans-serif; background: rgba(0,0,0,.78); color: #feca57;
    padding: 1px 3px; border-radius: 3px;
}
.att {
    position: absolute; left: -6px; bottom: 16%; width: calc(var(--w) * .42);
    height: calc(var(--w) * .59); border-radius: 3px; background: #2d3a5c center/cover;
    border: 1px solid #feca57; z-index: 3; box-shadow: 0 1px 3px #000;
}
.ctrlc0 .art { box-shadow: 0 0 0 2px #54a0ff; }
.ctrlc1 .art { box-shadow: 0 0 0 2px #ff6b6b; }
.gameover {
    padding: 8px; text-align: center; font-weight: 800; color: #00d2d3;
    border: 1px solid #00d2d3; border-radius: 6px; background: #00d2d310;
}
#card-details-pile { display: flex; flex-wrap: wrap; gap: 6px; justify-content: center; }
#card-details-pile:empty { display: none; }
#decision-panel pre { color: #feca57; }
#trace-panel pre { color: #a0a0a0; }
.trace-line { margin: 0; }
.trace-trc { color: #54a0ff; }
.trace-dbg { color: #ff9ff3; }
.trace-score { color: #00d2d3; font-weight: bold; }
.trace-kill { color: #ee5a24; }
.trace-effect { color: #7bed9f; }
.trace-combat { color: #f9ca24; font-weight: bold; }
.label { color: #666; font-size: 11px; text-transform: uppercase; margin-bottom: 4px; }
</style>
</head>
<body>
<div id="header">
    <h1 id="title">Riftbound Replay</h1>
    <div id="nav">
        <button onclick="go(-10)">&laquo;-10</button>
        <button onclick="go(-1)">&larr; Prev</button>
        <span id="counter">0 / 0</span>
        <button onclick="go(1)">Next &rarr;</button>
        <button onclick="go(10)">+10&raquo;</button>
    </div>
</div>
<div id="turn-info"></div>
<div id="main">
    <div id="board-panel">
        <div class="label board-label">Board State
            <span id="view-toggle" title="T switches"><button data-view="pic">Cards</button><button data-view="text">Text</button></span>
        </div>
        <div id="vboard"></div>
        <pre id="board"></pre>
    </div>
    <div class="resize-handle-v" data-target="board-panel" data-axis="x"></div>
    <div id="right-panel">
        <div id="decision-panel"><div class="label">Decision</div><pre id="decision"></pre></div>
        <div class="resize-handle-h" data-target="decision-panel" data-axis="y"></div>
        <div id="trace-panel"><div class="label">Trace Log</div><pre id="trace"></pre></div>
    </div>
    <div class="resize-handle-v" data-target="right-panel" data-axis="x"></div>
    <div id="card-details-panel">
        <div class="label">Card Details</div>
        <div id="card-details-name"></div>
        <img id="card-details-image" alt="" />
        <div id="card-details-placeholder">Click any card or card name to view its image.</div>
        <div id="card-details-pile"></div>
    </div>
</div>
<script>
)HTML";

    // Write snapshot data as JS array
    file << "const HEADER = \"" << escapeJs(game_header_) << "\";\n";
    file << "const ENGINE_VERSION = \"" << kVersionTag << "\";\n";
    file << "const ENGINE_BUILD = \"" << kBuildDate << "\";\n";

    // Card-name → official image URL. Populated from gallery_raw.json.
    // Used by the JS card-name wrapper to make every name in board /
    // decision / trace panels clickable; clicking sets the side-pane
    // image src to the URL below.
    {
        auto image_map = loadCardImageMap();
        file << "const CARD_IMAGES = {\n";
        bool first = true;
        for (const auto& [name, url] : image_map) {
            if (!first) file << ",\n";
            file << "  \"" << escapeJs(name) << "\":\"" << escapeJs(url) << "\"";
            first = false;
        }
        file << "\n};\n\n";

        file << "const CARDS = {\n";
        first = true;
        for (CardDefId id : used_defs_) {
            const auto& def = db_.get(id);
            std::string url = def.image_url;
            if (url.empty()) {
                auto it = image_map.find(def.name);
                if (it != image_map.end()) url = it->second;
            }
            if (!first) file << ",\n";
            file << "  " << id << ":{\"n\":\"" << escapeJs(def.name) << "\",\"i\":\""
                 << escapeJs(url) << "\",\"t\":\"" << toString(def.card_type) << "\"}";
            first = false;
        }
        file << "\n};\n\n";
    }

    file << "const SNAPSHOTS = [\n";

    for (size_t i = 0; i < snapshots_.size(); ++i) {
        auto& snap = snapshots_[i];
        file << "{\n";
        file << "  idx: " << snap.decision_index << ",\n";
        file << "  turn: \"" << escapeJs(snap.turn_info) << "\",\n";
        file << "  board: \"" << escapeJs(snap.board_ascii) << "\",\n";
        file << "  b: " << (snap.board_json.empty() ? std::string("null") : snap.board_json) << ",\n";
        file << "  decision: \"" << escapeJs(snap.decision_text) << "\",\n";
        file << "  trace: [";
        for (size_t j = 0; j < snap.trace_lines.size(); ++j) {
            if (j > 0) file << ",";
            file << "\"" << escapeJs(snap.trace_lines[j]) << "\"";
        }
        file << "],\n";
        file << "  v: " << snap.mcts_value
             << ", s: " << snap.mcts_sims
             << ", mp: " << snap.mcts_player
             << ", seam: " << (snap.is_seam ? 1 : 0)
             << ", phase: \"" << escapeJs(snap.seam_phase) << "\"\n";
        file << "}";
        if (i + 1 < snapshots_.size()) file << ",";
        file << "\n";
    }

    file << "];\n\n";

    // Write remaining trace lines (after last decision, e.g. game over)
    file << "const FINAL_TRACE = [";
    for (size_t i = 0; i < pending_trace_.size(); ++i) {
        if (i > 0) file << ",";
        file << "\"" << escapeJs(pending_trace_[i]) << "\"";
    }
    file << "];\n\n";

    file << R"HTML(
let pos = 0;

// Build a single regex that matches any known card name. Sort
// longer-first so "Ivern, Friend to All" doesn't get pre-empted by
// "Ivern". Each name is regex-escaped so commas / apostrophes /
// punctuation work as literals.
const CARD_NAMES_SORTED = Object.keys(CARD_IMAGES).sort((a, b) => b.length - a.length);
function escapeRegex(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }
const CARD_NAME_RE = CARD_NAMES_SORTED.length
    ? new RegExp('(' + CARD_NAMES_SORTED.map(escapeRegex).join('|') + ')', 'g')
    : null;

// Replace every known card-name occurrence in `elem`'s descendant text
// nodes with a clickable `<span class="card-link" data-card="…">`.
// Operates on text nodes only — preserves the surrounding <pre>
// whitespace and existing color spans in the trace panel. Safe to
// call repeatedly; new text replaces old DOM each show() cycle.
function wrapCardNames(elem) {
    if (!CARD_NAME_RE) return;
    const walker = document.createTreeWalker(elem, NodeFilter.SHOW_TEXT, null);
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
        const text = node.nodeValue;
        if (!CARD_NAME_RE.test(text)) continue;
        CARD_NAME_RE.lastIndex = 0; // reset for split
        const parts = text.split(CARD_NAME_RE);
        if (parts.length <= 1) continue;
        const frag = document.createDocumentFragment();
        for (const part of parts) {
            if (CARD_IMAGES.hasOwnProperty(part)) {
                const span = document.createElement('span');
                span.className = 'card-link';
                span.dataset.card = part;
                span.textContent = part;
                frag.appendChild(span);
            } else {
                frag.appendChild(document.createTextNode(part));
            }
        }
        node.parentNode.replaceChild(frag, node);
    }
}

// Pane resizing — handles between panels let the user drag to
// re-proportion. Vertical handles (data-axis="x") resize the panel
// to their LEFT by setting its explicit flex basis. Horizontal
// handles (data-axis="y") resize the panel ABOVE them likewise.
// No persistence — sizes reset each page load.
(function setupResize() {
    let drag = null;  // { handle, target, axis, startPos, startSize }
    document.addEventListener('pointerdown', function(e) {
        const handle = e.target.closest('.resize-handle-v, .resize-handle-h');
        if (!handle) return;
        const target = document.getElementById(handle.dataset.target);
        if (!target) return;
        const axis = handle.dataset.axis;
        const rect = target.getBoundingClientRect();
        drag = {
            handle: handle,
            target: target,
            axis: axis,
            startPos: axis === 'x' ? e.clientX : e.clientY,
            startSize: axis === 'x' ? rect.width : rect.height,
        };
        handle.classList.add('dragging');
        handle.setPointerCapture && handle.setPointerCapture(e.pointerId);
        document.body.classList.add(axis === 'x' ? 'dragging-pane' : 'dragging-pane-v');
        // The decision-panel has max-height: 40% baked into CSS;
        // override on drag so the user can grow it past that ceiling.
        if (axis === 'y') target.style.maxHeight = 'none';
        e.preventDefault();
    });
    document.addEventListener('pointermove', function(e) {
        if (!drag) return;
        const pos = drag.axis === 'x' ? e.clientX : e.clientY;
        const delta = pos - drag.startPos;
        const newSize = Math.max(80, drag.startSize + delta);
        // 80px minimum so a pane can't disappear; user can drag back.
        drag.target.style.flex = '0 0 ' + newSize + 'px';
    });
    document.addEventListener('pointerup', function() {
        if (!drag) return;
        drag.handle.classList.remove('dragging');
        document.body.classList.remove('dragging-pane', 'dragging-pane-v');
        drag = null;
    });
})();

// Single delegated click handler for all card-link spans across the
// three panels. Updates the side-pane image + name; tracks the active
// link with a class so the user can see what they just clicked.
// ── Picture view of the board ──────────────────────────────────────────
// Each snapshot's `b` is the board as JSON (ReplayWriter::boardJson);
// CARDS maps a card definition id to its name, image and type.
function thumb(url, w) {
    if (!url) return '';
    if (url.indexOf('rgpub.io/sanity') < 0) return url;  // only that CDN resizes
    return url + (url.indexOf('?') < 0 ? '?' : '&') + 'w=' + w + '&fm=webp';
}
function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
function cardInfo(o) {
    const d = (o.c !== undefined && CARDS[o.c]) || null;
    const name = o.n || (d ? d.n : '?');
    return { name: name, img: d ? d.i : (CARD_IMAGES[name] || ''), type: d ? d.t : '' };
}

// One card. opts: w (width px), land (landscape image), count (stack
// size), noTurn (draw exhausted cards upright), ctrl (controller tint).
let SCALE = 1;  // card sizes follow the board panel's width
function cardHtml(o, opts) {
    const w = Math.round(opts.w * SCALE);
    const ci = cardInfo(o);
    let cls = 'slot';
    if (o.x && !opts.noTurn) cls += ' exh';
    if (opts.land) cls += ' land';
    if (o.s) cls += ' sel';
    if (o.h) cls += ' fd';
    if (opts.ctrl === 0 || opts.ctrl === 1) cls += ' ctrlc' + opts.ctrl;
    const tw = opts.land ? 420 : (w > 70 ? 300 : 200);
    let html = ci.img
        ? '<div class="art" style="background-image:url(\'' + thumb(ci.img, tw) + '\')"></div>'
        : '<div class="art txt">' + esc(ci.name) + '</div>';
    const tip = [ci.name];
    if (o.m !== undefined) {
        const cls2 = o.m > o.bm ? ' up' : (o.m < o.bm ? ' down' : '');
        html += '<span class="badge b-might' + cls2 + '">' + o.m + '</span>';
        tip.push('Might ' + o.m + (o.m !== o.bm ? ' (printed ' + o.bm + ')' : ''));
    }
    if (o.d) { html += '<span class="badge b-dmg">' + o.d + ' dmg</span>'; tip.push(o.d + ' damage'); }
    if (o.cd) {
        html += '<span class="badge b-cd">' + (o.cd === 'A' ? '⚔ ATK' : '\u{1F6E1} DEF') + '</span>';
        tip.push(o.cd === 'A' ? 'Attacking' : 'Defending');
    }
    if (opts.count > 1) html += '<span class="badge b-count">×' + opts.count + '</span>';
    const tags = [];
    if (o.b) tags.push('+' + o.b);
    if (o.emp) tags.push('Empowered');
    if (o.tok) tags.push('Token');
    if (o.h) tags.push('Facedown');
    if (tags.length) { html += '<span class="badge b-tag">' + tags.join(' ') + '</span>'; tip.push(tags.join(', ')); }
    if (o.r) html += '<span class="badge b-res">resolving</span>';
    if (o.ab) tip.push('ability');
    if (o.k) {
        html += '<div class="kws">' + o.k.split(' ').map(k => '<span>' + esc(k) + '</span>').join('') + '</div>';
        tip.push(o.k);
    }
    if (o.x) tip.push('exhausted');
    for (const g of (o.e || [])) {
        const gi = cardInfo(g);
        html += '<div class="att" title="' + esc(gi.name) + '" data-card="' + esc(gi.name) +
                '" data-img="' + esc(gi.img) + '" style="background-image:url(\'' + thumb(gi.img, 120) + '\')"></div>';
        tip.push('equipped: ' + gi.name);
    }
    return '<div class="' + cls + '" style="--w:' + w + 'px" data-card="' + esc(ci.name) +
           '" data-img="' + esc(ci.img) + '" title="' + esc(tip.join(' · ')) + '">' + html + '</div>';
}
function cardsHtml(list, opts) { return list.map(o => cardHtml(o, opts)).join(''); }

// Runes: one card per kind and state, with a count.
function runesHtml(runes) {
    const groups = [];
    const byKey = {};
    for (const r of runes) {
        const key = (r.c !== undefined ? r.c : r.n) + (r.x ? 'x' : 'r');
        if (!(key in byKey)) { byKey[key] = groups.length; groups.push({ o: r, n: 0 }); }
        groups[byKey[key]].n++;
    }
    groups.sort((a, b) => (a.o.x ? 1 : 0) - (b.o.x ? 1 : 0));
    return groups.map(g => cardHtml(g.o, { w: 34, count: g.n })).join('');
}

// Trash or banishment: the top card with the count; click lists them all.
let PILES = {};
function pileHtml(key, label, list) {
    if (!list.length) return '<div class="zone"><div class="zl">' + label + '</div><div class="stat">0</div></div>';
    PILES[key] = { label: label, list: list };
    const top = list[list.length - 1];
    return '<div class="zone"><div class="zl">' + label + '</div><div class="pile" data-pile="' + key + '">' +
           cardHtml(Object.assign({}, top, { x: 0, s: 0 }), { w: 40, count: list.length }) + '</div></div>';
}

function playerStrip(b, seat) {
    const p = b.players[seat];
    const turn = b.tp === seat;
    let h = '<div class="pstrip p' + (seat + 1) + (turn ? ' turn' : '') + '">';
    h += '<div class="pname c' + seat + '">P' + (seat + 1) + (turn ? '<small>to play</small>' : '') + '</div>';
    h += '<div class="score">' + p.score + '<small>/ 8 pts</small></div>';
    if (p.legend) h += '<div class="zone"><div class="zl">Legend</div>' + cardHtml(p.legend, { w: 44 }) + '</div>';
    if (p.champ) h += '<div class="zone"><div class="zl">Champion</div>' + cardHtml(p.champ, { w: 44 }) + '</div>';
    h += '<div class="stat">Deck <b>' + p.deck + '</b><br>Runes left <b>' + p.rdeck + '</b><br>Pool <b>' +
         p.energy + '</b> E · <b>' + p.power + '</b> P' +
         (p.restricted ? '<br><small>' + p.restricted + '</small>' : '') + '</div>';
    h += '<div class="zone" style="flex:1"><div class="zl">Hand (' + p.hand.length + ')</div><div class="row">' +
         cardsHtml(p.hand, { w: 48, noTurn: true }) + '</div></div>';
    h += pileHtml('t' + seat, 'Trash', p.trash);
    h += pileHtml('b' + seat, 'Banished', p.ban);
    return h + '</div>';
}

function baseRow(b, seat) {
    const p = b.players[seat];
    let h = '<div class="base p' + (seat + 1) + '">';
    h += '<div class="zone"><div class="zl">Runes (' + p.runes.filter(r => !r.x).length + ' ready / ' +
         p.runes.length + ')</div><div class="row">' + runesHtml(p.runes) + '</div></div>';
    if (p.gear.length) h += '<div class="zone"><div class="zl">Gear</div><div class="row">' + cardsHtml(p.gear, { w: 50 }) + '</div></div>';
    h += '<div class="zone"><div class="zl">Base</div><div class="row">' +
         (p.units.length ? cardsHtml(p.units, { w: 62 }) : '<span class="stat">no units</span>') + '</div></div>';
    return h + '</div>';
}

function battlefieldsRow(b) {
    let h = '<div class="bfs">';
    for (const bf of b.bfs) {
        const ci = cardInfo(bf.card || {});
        const ctl = bf.ctrl >= 0 ? '<span class="ctl c' + bf.ctrl + '">P' + (bf.ctrl + 1) + ' controls</span>' : '';
        h += '<div class="bf' + (bf.ctrl >= 0 ? ' ctrl' + bf.ctrl : '') + (bf.flags ? ' hot' : '') + '">';
        const side = (seat, units) => '<div class="side s' + seat + '">' +
            cardsHtml(units, { w: 62 }) +
            cardsHtml(bf.fd.filter(f => f.ctrl === seat), { w: 44 }) + '</div>';
        h += side(0, bf.p1);
        h += '<div class="bfhead" data-card="' + esc(ci.name) + '" data-img="' + esc(ci.img) + '" ' +
             'style="background-image:url(\'' + thumb(ci.img, 480) + '\')">' +
             '<div class="bfname">' + esc(bf.name) + (bf.flags ? '<span class="bfflags">' + esc(bf.flags) + '</span>' : '') +
             ctl + '</div></div>';
        h += side(1, bf.p2);
        // Facedown cards whose controller isn't recorded go under the header.
        const other = bf.fd.filter(f => f.ctrl !== 0 && f.ctrl !== 1);
        if (other.length) h += '<div class="side">' + cardsHtml(other, { w: 44 }) + '</div>';
        h += '</div>';
    }
    return h + '</div>';
}

function chainRow(b) {
    if (!b.chain.length) return '';
    return '<div class="chainrow"><div class="zl">Chain<br>(top first)</div><div class="row">' +
        b.chain.map(o => cardHtml(o, { w: 50, ctrl: o.ctrl })).join('') + '</div></div>';
}

// Cards follow the board panel's width, then shrink until the board fits
// its height (down to 0.55; below that the panel scrolls).
function renderPictureBoard(s) {
    const el = document.getElementById('vboard');
    const panel = document.getElementById('board-panel');
    const b = s.b;
    PILES = {};
    if (!b) {
        el.innerHTML = '<div class="empty-note">No board yet. The decision panel shows the choice.</div>';
        return;
    }
    const avail = panel.getBoundingClientRect().bottom - el.getBoundingClientRect().top - 8;
    SCALE = Math.max(0.8, Math.min(2, el.clientWidth / 720));
    for (let pass = 0; pass < 4; pass++) {
        el.style.setProperty('--k', SCALE);
        PILES = {};
        let h = '';
        if (b.over) h += '<div class="gameover">GAME OVER — ' + esc(b.over) + '</div>';
        h += playerStrip(b, 0) + baseRow(b, 0) + chainRow(b) + battlefieldsRow(b) + baseRow(b, 1) + playerStrip(b, 1);
        el.innerHTML = h;
        const need = el.scrollHeight;
        if (need <= avail || SCALE <= 0.55) break;
        SCALE = Math.max(0.55, SCALE * avail / need);
    }
}

function setView(view) {
    document.body.classList.toggle('textview', view === 'text');
    for (const btn of document.querySelectorAll('#view-toggle button')) {
        btn.classList.toggle('on', btn.dataset.view === view);
    }
    try { localStorage.setItem('rb-replay-view', view); } catch (e) {}
}
// Redraw at the new size when the window or the board pane is resized.
let resizeTimer = null;
new ResizeObserver(function() {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function() { if (SNAPSHOTS.length) renderPictureBoard(SNAPSHOTS[pos]); }, 80);
}).observe(document.getElementById('board-panel'));
document.getElementById('view-toggle').addEventListener('click', function(e) {
    const btn = e.target.closest('button');
    if (btn) setView(btn.dataset.view);
});
(function() {
    let v = 'pic';
    try { v = localStorage.getItem('rb-replay-view') || 'pic'; } catch (e) {}
    setView(v);
})();

// Show a card (or a pile's list) in the details pane.
function showDetails(name, url, pile) {
    const img = document.getElementById('card-details-image');
    const nameLabel = document.getElementById('card-details-name');
    const placeholder = document.getElementById('card-details-placeholder');
    const pileEl = document.getElementById('card-details-pile');
    if (placeholder) placeholder.style.display = 'none';
    nameLabel.textContent = name;
    if (pile) {
        img.style.display = 'none';
        pileEl.innerHTML = pile.slice().reverse().map(o => cardHtml(Object.assign({}, o, { x: 0, s: 0 }), { w: 70 })).join('');
    } else {
        pileEl.innerHTML = '';
        img.src = url;
        img.alt = name;
        img.style.display = 'block';
    }
}

let activeLink = null;
document.addEventListener('click', function(e) {
    if (!e.target.closest) return;
    const pile = e.target.closest('[data-pile]');
    if (pile && PILES[pile.dataset.pile]) {
        const pl = PILES[pile.dataset.pile];
        showDetails(pl.label + ' (' + pl.list.length + ', newest first)', '', pl.list);
        return;
    }
    const pic = e.target.closest('[data-img]');
    if (pic && pic.dataset.img) {
        showDetails(pic.dataset.card, pic.dataset.img, null);
        return;
    }
    const link = e.target.closest('.card-link');
    if (!link) return;
    const name = link.dataset.card;
    const url = CARD_IMAGES[name];
    if (!url) return;
    showDetails(name, url, null);
    if (activeLink && activeLink !== link) activeLink.classList.remove('active');
    link.classList.add('active');
    activeLink = link;
});

function colorTrace(line) {
    let cls = 'trace-line';
    if (line.includes('SCORE') || line.includes('CONQUER')) cls = 'trace-score';
    else if (line.includes('KILL') || line.includes('BURN_OUT')) cls = 'trace-kill';
    else if (line.includes('EFFECT:')) cls = 'trace-effect';
    else if (line.includes('COMBAT') || line.includes('DAMAGE_STEP') || line.includes('SHOWDOWN')) cls = 'trace-combat';
    else if (line.includes('[DBG]')) cls = 'trace-dbg';
    else if (line.includes('[TRC]')) cls = 'trace-trc';
    return '<div class="' + cls + '">' + line.replace(/</g,'&lt;').replace(/>/g,'&gt;') + '</div>';
}

function show(i) {
    if (i < 0) i = 0;
    if (i >= SNAPSHOTS.length) i = SNAPSHOTS.length - 1;
    pos = i;
    const s = SNAPSHOTS[i];
    // Toggle the seam class on <body> so the CSS picks up the
    // distinctive yellow palette for phase-transition stops.
    document.body.classList.toggle('seam', !!s.seam);
    // Counter shows position + abbreviated turn context so you can tell
    // at a glance which turn/phase/player you're on without having to
    // read the turn-info bar. Parses the snapshot's turn string
    //   "Turn N | <Phase> | Pn's turn | …"
    // or the seam variant
    //   "Turn N | ▶ XxxPhase begins | Pn's turn | …"
    // into "T<N> P<n> <ShortPhase>".
    function shortTurn(turn) {
        const m = /Turn (\d+) \| ([^|]+) \| P(\d)/.exec(turn);
        if (!m) return '';
        const turnN = m[1];
        const p = m[3];
        let phase = m[2].trim();
        const seam = /^▶ (\w+?)(?:Phase|Step)? begins/.exec(phase);
        if (seam) phase = seam[1];
        else phase = phase.replace(/Phase$/, '').replace(/Step$/, '');
        return ' · T' + turnN + ' P' + p + ' ' + phase;
    }
    document.getElementById('counter').textContent =
        (i + 1) + ' / ' + SNAPSHOTS.length + shortTurn(s.turn);
    // Build the turn-info bar:
    //   [base turn info]  |  [optional SCORE DELTA banner]
    //
    // The score delta banner only appears when this snapshot's totals
    // differ from the previous one — i.e. a SCORE event fired in the
    // trace between them. Bright cyan, prefixed with "+N" so a reader
    // can scan the replay timeline visually to spot scoring moments
    // instead of grepping the trace for "[TRC] SCORE:".
    let turnLine = s.turn;
    // MCTS root-value badge — appears whenever the deciding agent was
    // MCTS (random-agent decisions store v = -2.0 as the "no estimate"
    // sentinel). v is the root mean reward from mcts_player's POV.
    // Both players' win-probs are computed in the zero-sum sense:
    //   P(win, mcts_player) = (v + 1) / 2
    //   P(win, opponent)    = 1 - that
    // Each side gets its own color-coded swatch so both trajectories
    // are visible side-by-side as you scrub through decisions.
    function colorFor(p) {
        if (p >= 0.7) return '#1dd1a1'; // green — winning
        if (p < 0.3)  return '#ee5253'; // red   — losing
        return '#feca57';               // yellow — uncertain
    }
    if (typeof s.v === 'number' && s.v >= -1.0 && (s.mp === 0 || s.mp === 1)) {
        const p_mcts = (s.v + 1) / 2;
        const p_opp  = 1 - p_mcts;
        const p1 = (s.mp === 0) ? p_mcts : p_opp;
        const p2 = (s.mp === 1) ? p_mcts : p_opp;
        const p1Str = (p1 * 100).toFixed(1);
        const p2Str = (p2 * 100).toFixed(1);
        turnLine += '  |  P(win) ' +
                    '<span style="color:' + colorFor(p1) + '; font-weight:bold;">P1=' + p1Str + '%</span>' +
                    ' / ' +
                    '<span style="color:' + colorFor(p2) + '; font-weight:bold;">P2=' + p2Str + '%</span>' +
                    ' <span style="color:#888;">[v=' + s.v.toFixed(3) +
                    ', sims=' + s.s +
                    ', deciding=P' + (s.mp + 1) + ']</span>';
    }
    if (i > 0) {
        // Parse "Score: P1=X P2=Y" out of the turn strings and compare.
        const re = /P1=(\d+)\s+P2=(\d+)/;
        const cur = re.exec(s.turn);
        const prev = re.exec(SNAPSHOTS[i - 1].turn);
        if (cur && prev) {
            const d1 = parseInt(cur[1]) - parseInt(prev[1]);
            const d2 = parseInt(cur[2]) - parseInt(prev[2]);
            const parts = [];
            if (d1 > 0) parts.push('P1 +' + d1);
            if (d2 > 0) parts.push('P2 +' + d2);
            if (parts.length > 0) {
                turnLine += '  |  <span style="color:#00d2d3; font-weight:bold;">' +
                            '★ SCORE: ' + parts.join(', ') + '</span>';
            }
        }
    }
    document.getElementById('turn-info').innerHTML = turnLine;
    document.getElementById('board').textContent = s.board;
    renderPictureBoard(s);
    document.getElementById('decision').textContent = s.decision || '(no decision recorded)';
    wrapCardNames(document.getElementById('board'));
    wrapCardNames(document.getElementById('decision'));

    // Show trace lines caused BY this snapshot's chosen action — the
    // events fired between this decision being applied and the next
    // decision prompting (effect resolutions, peeks, reveals, scoring,
    // etc.). Each decision's log thus aligns with its own consequences.
    // Snapshot 0 additionally carries any pre-game setup trace
    // (mulligan, opening hand) since it has no predecessor.
    let traceLines = s.trace || [];
    // Append FINAL_TRACE on the very last snapshot so end-of-game events
    // (final scoring, GAME_OVER) still show up.
    if (i === SNAPSHOTS.length - 1) {
        traceLines = traceLines.concat(FINAL_TRACE);
    }
    document.getElementById('trace').innerHTML = traceLines.map(colorTrace).join('');
    wrapCardNames(document.getElementById('trace'));
}

function go(delta) { show(pos + delta); }

// Skip over consecutive seams in one direction so users who don't
// want to stop on every phase boundary can still scrub quickly with
// Shift+Arrow.
function goSkipSeams(dir) {
    let i = pos + dir;
    while (i >= 0 && i < SNAPSHOTS.length && SNAPSHOTS[i].seam) {
        i += dir;
    }
    show(i);
}

document.addEventListener('keydown', function(e) {
    if (e.shiftKey && (e.key === 'ArrowRight' || e.key === 'L')) goSkipSeams(1);
    else if (e.shiftKey && (e.key === 'ArrowLeft' || e.key === 'H')) goSkipSeams(-1);
    else if (e.key === 'ArrowRight' || e.key === 'l') go(1);
    else if (e.key === 'ArrowLeft' || e.key === 'h') go(-1);
    else if (e.key === 'Home') show(0);
    else if (e.key === 'End') show(SNAPSHOTS.length - 1);
    else if (e.key === 'PageDown') go(10);
    else if (e.key === 'PageUp') go(-10);
    else if (e.key === 't' || e.key === 'T') setView(document.body.classList.contains('textview') ? 'pic' : 'text');
});

document.getElementById('title').textContent =
    'Riftbound Replay — ' + HEADER + '  [engine ' + ENGINE_VERSION +
    ', built ' + ENGINE_BUILD + ']';
document.getElementById('title').title = document.getElementById('title').textContent;
show(0);
</script>
</body>
</html>
)HTML";

    file.close();
}

} // namespace riftbound
