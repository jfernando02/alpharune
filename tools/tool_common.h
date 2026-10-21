#pragma once
/// @file tool_common.h
/// Helpers shared by the command-line tools in tools/: the legal decks of a
/// directory, and the agents the tools can field.

#include "agents/agent_interface.h"
#include "agents/gbt_evaluator.h"
#include "agents/heuristic_agent.h"
#include "agents/random_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "rules/deck_validator.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace riftbound::tools {

struct DeckSet {
    std::vector<std::string> names;  // file stems, sorted, so indices are stable
    std::vector<DeckSubmission> decks;
};

/// Every legal deck list (*.txt) in `dir`; illegal or unreadable ones are
/// skipped with a note on stderr.
inline DeckSet loadLegalDecks(const std::string& dir, const CardDB& db) {
    namespace fs = std::filesystem;
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ".txt") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());

    DeckSet out;
    DeckValidator validator(db);
    for (const auto& p : paths) {
        try {
            DeckSubmission d = DeckValidator::loadFromDeckList(p.string(), db);
            if (!validator.validate(d).is_legal) {
                std::cerr << "skipping illegal deck " << p.filename().string() << "\n";
                continue;
            }
            out.names.push_back(p.stem().string());
            out.decks.push_back(std::move(d));
        } catch (const std::exception& e) {
            std::cerr << "skipping " << p.filename().string() << ": " << e.what() << "\n";
        }
    }
    return out;
}

/// An agent a tool can field, parsed from
///   random | scripted | heuristic[:model=FILE,guesses=N,turns_ahead=N,step_budget=N,
///                                   <weight>=X]
/// where <weight> is any EvalWeights field (agents/evaluator.h). A model is
/// loaded once here and shared by every game (models are read-only).
struct ToolAgentSpec {
    std::string kind = "heuristic";
    HeuristicConfig heuristic;  // kind == "heuristic"; the seed is set per game
};

inline ToolAgentSpec parseToolAgent(const std::string& spec) {
    ToolAgentSpec out;
    const auto colon = spec.find(':');
    out.kind = spec.substr(0, colon);
    if (out.kind != "random" && out.kind != "scripted" && out.kind != "heuristic")
        throw std::runtime_error("unknown agent '" + out.kind + "' in '" + spec + "'");
    if (colon == std::string::npos) return out;
    if (out.kind != "heuristic")
        throw std::runtime_error("agent '" + out.kind + "' takes no options");

    std::stringstream opts(spec.substr(colon + 1));
    for (std::string kv; std::getline(opts, kv, ',');) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) throw std::runtime_error("expected key=value, got '" + kv + "'");
        const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
        if (key == "model") out.heuristic.model = GbtModel::load(val);
        else if (key == "guesses") out.heuristic.guesses = std::stoi(val);
        else if (key == "turns_ahead") out.heuristic.turns_ahead = std::stoi(val);
        else if (key == "step_budget") out.heuristic.max_rollout_steps_per_turn = std::stol(val);
        else if (!setWeight(out.heuristic.weights, key, std::stod(val)))
            throw std::runtime_error("unknown heuristic option '" + key + "'");
    }
    return out;
}

inline std::unique_ptr<AgentInterface> makeToolAgent(const ToolAgentSpec& spec, const CardDB& db,
                                                     const CardRegistry& reg, uint64_t seed) {
    if (spec.kind == "random") return std::make_unique<RandomAgent>(seed | 1);
    if (spec.kind == "scripted") return std::make_unique<ScriptedAgent>(db);
    HeuristicConfig config = spec.heuristic;
    config.seed = seed | 1;
    return std::make_unique<HeuristicAgent>(db, reg, config);
}

} // namespace riftbound::tools
