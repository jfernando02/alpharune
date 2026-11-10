#pragma once
/// @file policy_agent.h
/// Plays the learned policy's top action with no search: ScriptedAgent's
/// answer at the decision kinds the policy does not cover.
///
/// A game costs milliseconds per decision instead of seconds, so it is the
/// cheap source of positions for the value model (tools/value_data.cpp with
/// --agent policy:...) and a quick first check of a new policy. For varied
/// positions the main-phase and Showdown decisions can be sampled: with
/// probability `epsilon` a random legal action, otherwise (temperature > 0)
/// an action drawn from softmax(scores / temperature).

#include "agent_interface.h"
#include "policy_model.h"
#include "scripted_agent.h"

#include "ml/policy_features.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>

namespace riftbound {

struct PolicyAgentConfig {
    std::shared_ptr<const PolicyModel> policy;
    double temperature = 0.0;
    double epsilon = 0.0;
    uint64_t seed = 1;
};

class PolicyAgent : public AgentInterface {
public:
    PolicyAgent(const CardDB& card_db, PolicyAgentConfig config)
        : card_db_(card_db), config_(std::move(config)), scripted_(card_db), rng_(config_.seed | 1) {}

    /// Decisions in one turn after which the agent passes whenever it can.
    /// The policy has no notion of "enough": with a repeatable free action
    /// (or a card that wrongly allows one) it can choose it forever, and the
    /// game never ends. Normal turns take well under 100 decisions.
    static constexpr int kMaxDecisionsPerTurn = 400;

    Intent selectAction(const GameState& state, const std::vector<Intent>& legal) override {
        if (legal.empty()) return Intent::concede(state.turn.turn_player);
        if (state.turn.turn_number != turn_) {
            turn_ = state.turn.turn_number;
            decisions_this_turn_ = 0;
        }
        if (++decisions_this_turn_ > kMaxDecisionsPerTurn) {
            ++capped_decisions_;
            for (IntentType pass : {IntentType::PassFocus, IntentType::PassPriority, IntentType::EndTurn})
                for (const Intent& i : legal)
                    if (i.type == pass) return i;
        }
        if (legal.size() == 1) return legal[0];
        const int scripted = scripted_.selectIndex(state, legal);
        const char* kind = ml::policyDecisionKind(state, legal);
        if (!config_.policy || !config_.policy->covers(kind)) return legal[scripted < 0 ? 0 : scripted];

        std::vector<int> options;
        for (size_t i = 0; i < legal.size(); ++i)
            if (legal[i].type != IntentType::Concede) options.push_back(static_cast<int>(i));
        if (options.empty()) return legal[0];
        const bool explore = std::strcmp(kind, "main") == 0 || std::strcmp(kind, "showdown") == 0;
        if (explore && config_.epsilon > 0.0 &&
            std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < config_.epsilon)
            return legal[options[std::uniform_int_distribution<size_t>(0, options.size() - 1)(rng_)]];

        const std::vector<double> scores = config_.policy->scores(state, card_db_, legal, kind, scripted);
        int best = options[0];
        for (int i : options)
            if (scores[i] > scores[best]) best = i;
        if (!explore || config_.temperature <= 0.0) return legal[best];

        std::vector<double> weights;
        weights.reserve(options.size());
        for (int i : options) weights.push_back(std::exp((scores[i] - scores[best]) / config_.temperature));
        return legal[options[std::discrete_distribution<size_t>(weights.begin(), weights.end())(rng_)]];
    }

    /// Decisions answered by passing because of kMaxDecisionsPerTurn.
    long cappedDecisions() const { return capped_decisions_; }

private:
    const CardDB& card_db_;
    PolicyAgentConfig config_;
    ScriptedAgent scripted_;
    std::mt19937_64 rng_;
    int turn_ = -1;
    int decisions_this_turn_ = 0;
    long capped_decisions_ = 0;
};

} // namespace riftbound
