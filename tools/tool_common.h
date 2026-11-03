#pragma once
/// @file tool_common.h
/// Helpers shared by the command-line tools in tools/: the legal decks of a
/// directory, and the agents the tools can field.

#include "agents/agent_interface.h"
#include "agents/gbt_evaluator.h"
#include "agents/heuristic_agent.h"
#include "agents/ismcts_bot.h"
#include "agents/random_agent.h"
#include "agents/scripted_agent.h"
#include "cards/card_registry.h"
#include "core/card_db.h"
#include "engine/game_engine.h"
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
///   random | scripted
///   | heuristic[:model=FILE,guesses=N,turns_ahead=N,step_budget=N,showdowns=0/1,followups=N,
///                chain=0/1,setup=0/1,setup_guesses=N,setup_turns=N,<weight>=X]
///   | ismcts[:sims=N,uct_c=X,turns_ahead=N,step_budget=N,showdowns=0/1,model=FILE,<weight>=X]
/// where <weight> is any EvalWeights field (agents/evaluator.h). A model is
/// loaded once here and shared by every game (models are read-only).
/// Either look-ahead agent also takes offsearch=random[:KIND]: the decisions
/// it does not search (all of them, or only those of one decisionKind) are
/// made at random instead of by ScriptedAgent, which measures how much those
/// decisions matter.
struct ToolAgentSpec {
    std::string kind = "heuristic";
    HeuristicConfig heuristic;  // kind == "heuristic"; the seed is set per game
    IsmctsConfig ismcts;        // kind == "ismcts"; likewise
    bool offsearch_random = false;
    std::string offsearch_kind;  // "" = every unsearched decision
};

/// What kind of decision `legal` is, by the actions on offer:
///   main (main-phase priority, resumable), main_mid (asked partway through a
///   main-phase action), mulligan, damage (combat damage assignment),
///   showdown (focus: play or pass), chain (react or pass, resumable),
///   chain_mid (react or pass where a resumed engine cannot re-enter), trigger
///   (optional trigger), trigger_cost, battlefield, rune (rune payment),
///   resolve (a choice while a chain item resolves: target / mode / X),
///   choice (any other MakeChoice: play location, optional cost, ...).
inline bool offers(const std::vector<Intent>& legal, IntentType t) {
    return std::any_of(legal.begin(), legal.end(), [t](const Intent& a) { return a.type == t; });
}
inline std::string decisionKind(const GameState& s, const std::vector<Intent>& legal) {
    if (HeuristicAgent::isLookaheadPoint(s, legal)) return "main";
    if (offers(legal, IntentType::EndTurn)) return "main_mid";
    if (offers(legal, IntentType::MulliganDecision)) return "mulligan";
    if (offers(legal, IntentType::AssignCombatDamage)) return "damage";
    if (offers(legal, IntentType::PassFocus)) return "showdown";
    if (offers(legal, IntentType::PassPriority)) return isCleanChainPoint(s) ? "chain" : "chain_mid";
    if (offers(legal, IntentType::PlaceOptionalTrigger) || offers(legal, IntentType::DeclineOptionalTrigger))
        return "trigger";
    if (offers(legal, IntentType::PayTriggeredCost) || offers(legal, IntentType::DeclineTriggeredCost))
        return "trigger_cost";
    if (offers(legal, IntentType::ChooseBattlefield)) return "battlefield";
    if (offers(legal, IntentType::MakeChoice)) {
        if (s.cost_cursor.has_value()) return "rune";
        if (s.chain.exists() || s.chain.resuming.has_value()) return "resolve";
        return "choice";
    }
    return std::string("other:") + toString(legal[0].type);
}

/// A look-ahead agent whose unsearched decisions (of one kind, or all) are
/// made at random.
class OffSearchRandomAgent : public AgentInterface {
public:
    OffSearchRandomAgent(std::unique_ptr<AgentInterface> inner, uint64_t seed, std::string kind)
        : inner_(std::move(inner)), random_(seed | 1), kind_(std::move(kind)) {}
    Intent selectAction(const GameState& s, const std::vector<Intent>& legal) override {
        if (legal.size() >= 2 && !HeuristicAgent::isLookaheadPoint(s, legal) &&
            (kind_.empty() || decisionKind(s, legal) == kind_))
            return random_.selectAction(s, legal);
        return inner_->selectAction(s, legal);
    }
    const AgentInterface& inner() const { return *inner_; }

private:
    std::unique_ptr<AgentInterface> inner_;
    RandomAgent random_;
    std::string kind_;
};

inline ToolAgentSpec parseToolAgent(const std::string& spec) {
    ToolAgentSpec out;
    const auto colon = spec.find(':');
    out.kind = spec.substr(0, colon);
    if (out.kind != "random" && out.kind != "scripted" && out.kind != "heuristic" &&
        out.kind != "ismcts")
        throw std::runtime_error("unknown agent '" + out.kind + "' in '" + spec + "'");
    if (colon == std::string::npos) return out;
    if (out.kind == "random" || out.kind == "scripted")
        throw std::runtime_error("agent '" + out.kind + "' takes no options");

    const bool h = out.kind == "heuristic";
    std::stringstream opts(spec.substr(colon + 1));
    for (std::string kv; std::getline(opts, kv, ',');) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) throw std::runtime_error("expected key=value, got '" + kv + "'");
        const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
        if (key == "offsearch") {
            const std::string how = val.substr(0, val.find(':'));
            if (how != "random" && how != "scripted")
                throw std::runtime_error("offsearch must be random[:KIND] or scripted");
            out.offsearch_random = how == "random";
            if (val.find(':') != std::string::npos) out.offsearch_kind = val.substr(val.find(':') + 1);
        }
        else if (key == "model") (h ? out.heuristic.model : out.ismcts.model) = GbtModel::load(val);
        else if (key == "turns_ahead") (h ? out.heuristic.turns_ahead : out.ismcts.turns_ahead) = std::stoi(val);
        else if (key == "step_budget" && h) out.heuristic.max_rollout_steps_per_turn = std::stol(val);
        else if (key == "step_budget") out.ismcts.max_steps_per_turn = std::stol(val);
        else if (key == "showdowns") (h ? out.heuristic.search_showdowns : out.ismcts.search_showdowns) = std::stoi(val) != 0;
        else if (key == "guesses" && h) out.heuristic.guesses = std::stoi(val);
        else if (key == "followups" && h) out.heuristic.plan_followups = std::stoi(val);
        else if (key == "chain" && h) out.heuristic.search_chain = std::stoi(val) != 0;
        else if (key == "setup" && h) out.heuristic.search_setup = std::stoi(val) != 0;
        else if (key == "setup_guesses" && h) out.heuristic.setup_guesses = std::stoi(val);
        else if (key == "setup_turns" && h) out.heuristic.setup_turns = std::stoi(val);
        else if (key == "sims" && !h) out.ismcts.max_simulations = std::stoi(val);
        else if (key == "uct_c" && !h) out.ismcts.uct_c = std::stod(val);
        else if (!setWeight(h ? out.heuristic.weights : out.ismcts.weights, key, std::stod(val)))
            throw std::runtime_error("unknown " + out.kind + " option '" + key + "'");
    }
    return out;
}

inline std::unique_ptr<AgentInterface> makeToolAgent(const ToolAgentSpec& spec, const CardDB& db,
                                                     const CardRegistry& reg, uint64_t seed) {
    if (spec.kind == "random") return std::make_unique<RandomAgent>(seed | 1);
    if (spec.kind == "scripted") return std::make_unique<ScriptedAgent>(db);
    std::unique_ptr<AgentInterface> agent;
    if (spec.kind == "ismcts") {
        IsmctsConfig config = spec.ismcts;
        config.seed = seed | 1;
        agent = std::make_unique<IsmctsBot>(db, reg, config);
    } else {
        HeuristicConfig config = spec.heuristic;
        config.seed = seed | 1;
        agent = std::make_unique<HeuristicAgent>(db, reg, config);
    }
    if (spec.offsearch_random)
        return std::make_unique<OffSearchRandomAgent>(std::move(agent), seed ^ 0x9E3779B97F4A7C15ULL,
                                                      spec.offsearch_kind);
    return agent;
}

/// Searched decisions and budget fallbacks of a look-ahead agent (zeros for
/// the others), for the tools' reports.
struct SearchCounts { long searched = 0, fallbacks = 0; };
inline SearchCounts searchCounts(const AgentInterface& agent) {
    if (const auto* w = dynamic_cast<const OffSearchRandomAgent*>(&agent))
        return searchCounts(w->inner());
    if (const auto* h = dynamic_cast<const HeuristicAgent*>(&agent))
        return {h->stats().lookahead_decisions, h->stats().budget_fallbacks};
    if (const auto* m = dynamic_cast<const IsmctsBot*>(&agent))
        return {m->stats().searched_decisions, m->stats().budget_fallbacks};
    return {};
}

} // namespace riftbound::tools
