#include "heuristic_agent.h"

#include "determinize.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace riftbound {

HeuristicAgent::HeuristicAgent(const CardDB& card_db,
                               const CardRegistry& card_registry,
                               HeuristicConfig config)
    : card_db_(card_db),
      card_registry_(card_registry),
      config_(config),
      scripted_(card_db),
      rng_(config.seed == 0 ? std::random_device{}() : config.seed) {}

bool HeuristicAgent::isLookaheadPoint(const GameState& state,
                                      const std::vector<Intent>& legal) {
    if (!isCleanResumePoint(state)) return false;
    return std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::EndTurn;
    });
}

Intent HeuristicAgent::selectAction(const GameState& state,
                                    const std::vector<Intent>& legal) {
    if (legal.empty()) return Intent::concede(state.turn.turn_player);
    if (legal.size() == 1) return legal[0];
    ++stats_.decisions;

    if (!isLookaheadPoint(state, legal)) return scripted_.selectAction(state, legal);
    ++stats_.lookahead_decisions;

    if (state.turn.turn_number != lookahead_turn_) {
        lookahead_turn_ = state.turn.turn_number;
        lookaheads_this_turn_ = 0;
        steps_this_turn_ = 0;
    }
    if (++lookaheads_this_turn_ > config_.max_lookaheads_per_turn) {
        // isLookaheadPoint guarantees an EndTurn is present.
        ++stats_.turn_cap_hits;
        return *std::find_if(legal.begin(), legal.end(), [](const Intent& a) {
            return a.type == IntentType::EndTurn;
        });
    }

    // Fit the search to what is left of the turn's budget, pricing a
    // simulation at the average rollout so far (a guess before the first).
    int guesses = config_.guesses;
    if (config_.max_rollout_steps_per_turn > 0) {
        const long left = config_.max_rollout_steps_per_turn - steps_this_turn_;
        const double per_sim = stats_.simulations > 0
            ? static_cast<double>(stats_.rollout_steps) / stats_.simulations : 20.0;
        const long affordable = static_cast<long>(left / (per_sim * legal.size()));
        if (affordable < 1) {
            ++stats_.budget_fallbacks;
            return scripted_.selectAction(state, legal);
        }
        if (affordable < guesses) {
            guesses = static_cast<int>(affordable);
            ++stats_.thinned_searches;
        }
    }

    const PlayerId me = legal[0].player;
    const size_t n = legal.size();
    std::vector<double> total(n, 0.0);
    std::vector<int> samples(n, 0);

    for (int g = 0; g < guesses; ++g) {
        GameState guess = state;
        determinize(guess, me, rng_);
        // Nonzero: resumeFromSnapshot treats 0 as "seed from random_device".
        const uint64_t engine_seed = rng_() | 1;
        for (size_t i = 0; i < n; ++i) {
            if (legal[i].type == IntentType::Concede) continue;
            if (auto score = simulate(guess, legal[i], me, engine_seed)) {
                total[i] += *score;
                ++samples[i];
            }
        }
    }

    size_t best = n;
    double best_score = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < n; ++i) {
        if (samples[i] == 0) continue;
        const double mean = total[i] / samples[i];
        if (mean > best_score) {
            best_score = mean;
            best = i;
        }
    }
    // Every simulation failed: fall back rather than guess.
    if (best == n) return scripted_.selectAction(state, legal);
    if (legal[best].type == IntentType::EndTurn) {
        ++stats_.lookahead_end_turns;
        countUnspentEnergy(state, legal, me);
    }
    return legal[best];
}

void HeuristicAgent::countUnspentEnergy(const GameState& state,
                                        const std::vector<Intent>& legal,
                                        PlayerId me) {
    const bool can_play = std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::PlayCard;
    });
    const auto runes = state.runesInBase(me);
    const bool rune_ready = std::any_of(runes.begin(), runes.end(), [&](GameObjectId id) {
        return !state.getObject(id).is_exhausted;
    });
    if (!can_play || !rune_ready) return;
    ++stats_.end_turn_unspent;
    const auto& hand = state.player(me).hand;
    if (std::any_of(hand.begin(), hand.end(), [&](GameObjectId id) {
            const auto& k = state.getObject(id).keywords;
            return k.has(Keyword::Action) || k.has(Keyword::Reaction);
        }))
        ++stats_.end_turn_unspent_holding_instant;
}

std::optional<double> HeuristicAgent::simulate(const GameState& guess,
                                               const Intent& action,
                                               PlayerId me,
                                               uint64_t engine_seed) {
    try {
        EventBus bus;
        GameEngine engine(card_db_, bus, card_registry_);
        StepResult step = engine.resumeFromSnapshot(guess, engine_seed);

        int index = -1;
        if (step.kind == StepKind::NeedDecision) {
            for (size_t j = 0; j < step.legal.size(); ++j) {
                if (step.legal[j] == action) {
                    index = static_cast<int>(j);
                    break;
                }
            }
        }
        if (index < 0) {
            ++stats_.failed_simulations;
            return std::nullopt;
        }

        const int last_turn = guess.turn.turn_number + config_.turns_ahead - 1;
        step = engine.applyChoice(index);
        int s = 0;
        for (; s < config_.max_rollout_steps &&
               step.kind == StepKind::NeedDecision &&
               engine.state().turn.turn_number <= last_turn;
             ++s) {
            const int pick = scripted_.selectIndex(engine.state(), step.legal);
            step = engine.applyChoice(pick < 0 ? 0 : pick);
        }
        stats_.rollout_steps += s + 1;  // + the action itself
        steps_this_turn_ += s + 1;

        ++stats_.simulations;
        const GameState& end = engine.state();
        if (!config_.model) return evaluate(end, me, config_.weights);
        if (end.game_over) return end.winner == me ? 1.0 : end.winner == opponent(me) ? 0.0 : 0.5;
        return config_.model->winProbability(end, me);
    } catch (const std::exception&) {
        ++stats_.failed_simulations;
        return std::nullopt;
    }
}

} // namespace riftbound
