#include "heuristic_agent.h"

#include "determinize.h"
#include "heuristic_tree.h"

#include "core/events.h"
#include "engine/game_engine.h"

#include <algorithm>
#include <cmath>
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

HeuristicAgent::~HeuristicAgent() = default;

namespace {

int indexOf(const std::vector<Intent>& legal, const Intent& action) {
    for (size_t j = 0; j < legal.size(); ++j)
        if (legal[j] == action) return static_cast<int>(j);
    return -1;
}

} // namespace

bool HeuristicAgent::isLookaheadPoint(const GameState& state,
                                      const std::vector<Intent>& legal) {
    if (!isCleanResumePoint(state)) return false;
    return std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::EndTurn;
    });
}

bool HeuristicAgent::isShowdownLookaheadPoint(const GameState& state,
                                              const std::vector<Intent>& legal) {
    if (!isCleanShowdownPoint(state)) return false;
    return std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::PassFocus;
    });
}

bool HeuristicAgent::isChainLookaheadPoint(const GameState& state,
                                           const std::vector<Intent>& legal) {
    if (!isCleanChainPoint(state)) return false;
    return std::any_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::PassPriority;
    });
}

bool HeuristicAgent::isSetupLookaheadPoint(const GameState& state,
                                           const std::vector<Intent>& legal) {
    if (!isCleanSetupPoint(state)) return false;
    return std::all_of(legal.begin(), legal.end(), [](const Intent& a) {
        return a.type == IntentType::ChooseBattlefield || a.type == IntentType::MulliganDecision;
    });
}

int HeuristicAgent::defaultIndex(const GameState& state, const std::vector<Intent>& legal,
                                 bool use_policy) const {
    const int scripted = scripted_.selectIndex(state, legal);
    if (use_policy && config_.policy) {
        const int pick = config_.policy->pick(state, card_db_, legal, scripted);
        if (pick >= 0) return pick;
    }
    return scripted;
}

Intent HeuristicAgent::defaultAction(const GameState& state, const std::vector<Intent>& legal) const {
    const int i = defaultIndex(state, legal, config_.policy_offsearch);
    return legal[i < 0 ? 0 : i];
}

HeuristicAgent::SearchKind HeuristicAgent::searchKind(const GameState& state,
                                                      const std::vector<Intent>& legal) const {
    if (isLookaheadPoint(state, legal)) return SearchKind::Main;
    if (config_.search_showdowns && isShowdownLookaheadPoint(state, legal))
        return SearchKind::Showdown;
    if (config_.search_chain && isChainLookaheadPoint(state, legal)) return SearchKind::Chain;
    if (isSetupLookaheadPoint(state, legal) &&
        (legal[0].type == IntentType::ChooseBattlefield ? config_.search_battlefield
                                                        : config_.search_mulligan))
        return SearchKind::Setup;
    return SearchKind::None;
}

int HeuristicAgent::lastTurn(const GameState& state) const {
    if (isCleanSetupPoint(state)) return config_.setup_turns - 1;
    return state.turn.turn_number + config_.turns_ahead - 1;
}

Intent HeuristicAgent::selectAction(const GameState& state,
                                    const std::vector<Intent>& legal) {
    if (legal.empty()) return Intent::concede(state.turn.turn_player);
    if (legal.size() == 1) return legal[0];
    ++stats_.decisions;

    const SearchKind kind = searchKind(state, legal);
    const bool searchable = kind != SearchKind::None;

    // The follow-ups of the action last chosen, in the order it planned them.
    if (!plan_.empty()) {
        if (searchable) {
            plan_.clear();  // the action is over
            plan_scores_.clear();
        } else {
            const int i = indexOf(legal, plan_.front());
            if (i >= 0) {
                if (observer_) {
                    std::vector<double> scores(legal.size(), std::nan(""));
                    int known = 0;
                    for (const auto& [option, score] : plan_scores_.front()) {
                        const int j = indexOf(legal, option);
                        if (j >= 0) { scores[j] = score; ++known; }
                    }
                    if (known >= 2) observer_(state, legal, scores);
                }
                plan_.erase(plan_.begin());
                plan_scores_.erase(plan_scores_.begin());
                ++stats_.planned_followups;
                return legal[i];
            }
            plan_.clear();
            plan_scores_.clear();
            ++stats_.abandoned_plans;
        }
    }
    if (!searchable) {
        if (config_.tree)
            if (auto answer = treeAnswer(state, legal)) return *answer;
        return defaultAction(state, legal);
    }
    ++stats_.lookahead_decisions;
    if (kind == SearchKind::Showdown) ++stats_.showdown_searches;
    if (kind == SearchKind::Chain) ++stats_.chain_searches;
    if (kind == SearchKind::Setup) ++stats_.setup_searches;

    // Setup decisions have a budget of their own (turn "-1"), so they do not
    // eat into the first turn's.
    const int turn_key = kind == SearchKind::Setup ? -1 : state.turn.turn_number;
    if (turn_key != lookahead_turn_) {
        lookahead_turn_ = turn_key;
        lookaheads_this_turn_ = 0;
        steps_this_turn_ = 0;
    }
    if (++lookaheads_this_turn_ > config_.max_lookaheads_per_turn) {
        ++stats_.turn_cap_hits;
        // A main-phase decision ends the turn; any other is answered as
        // ScriptedAgent (it has no EndTurn).
        const auto end = std::find_if(legal.begin(), legal.end(), [](const Intent& a) {
            return a.type == IntentType::EndTurn;
        });
        return end != legal.end() ? *end : defaultAction(state, legal);
    }

    const PlayerId me = legal[0].player;
    if (config_.tree && kind != SearchKind::Setup) return treeSearch(state, legal, me);

    // The first guess, drawn before the budget check so the plans can be
    // found in it (follow-ups are about public objects: runes, targets).
    GameState first_guess = state;
    determinize(first_guess, me, rng_);
    // Nonzero: resumeFromSnapshot treats 0 as "seed from random_device".
    const uint64_t first_seed = rng_() | 1;

    // Candidate plans: each legal action, with its follow-ups if planned.
    std::vector<std::vector<Intent>> plans;
    for (const Intent& action : legal) {
        if (action.type == IntentType::Concede) continue;
        if (config_.plan_followups > 0)
            expandPlans(first_guess, first_seed, me, {action}, plans);
        else
            plans.push_back({action});
    }
    if (plans.empty()) return defaultAction(state, legal);

    // Fit the search to what is left of the turn's budget, pricing a
    // simulation at the average rollout so far (a guess before the first).
    int guesses = kind == SearchKind::Setup ? config_.setup_guesses : config_.guesses;
    if (config_.max_rollout_steps_per_turn > 0) {
        const long left = config_.max_rollout_steps_per_turn - steps_this_turn_;
        const double per_sim = stats_.simulations > 0
            ? static_cast<double>(stats_.rollout_steps) / stats_.simulations : 20.0;
        const long affordable = static_cast<long>(left / (per_sim * plans.size()));
        if (affordable < 1) {
            ++stats_.budget_fallbacks;
            return defaultAction(state, legal);
        }
        if (affordable < guesses) {
            guesses = static_cast<int>(affordable);
            ++stats_.thinned_searches;
        }
    }

    const size_t n = plans.size();
    std::vector<double> total(n, 0.0);
    std::vector<int> samples(n, 0);

    for (int g = 0; g < guesses; ++g) {
        GameState guess;
        uint64_t engine_seed;
        if (g == 0) {
            guess = first_guess;
            engine_seed = first_seed;
        } else {
            guess = state;
            determinize(guess, me, rng_);
            engine_seed = rng_() | 1;
        }
        for (size_t i = 0; i < n; ++i) {
            if (auto score = simulate(guess, plans[i], me, engine_seed)) {
                total[i] += *score;
                ++samples[i];
            }
        }
    }

    size_t best = n;
    double best_score = -std::numeric_limits<double>::infinity();
    std::vector<double> mean(n, std::nan(""));
    for (size_t i = 0; i < n; ++i) {
        if (samples[i] == 0) continue;
        mean[i] = total[i] / samples[i];
        if (mean[i] > best_score) {
            best_score = mean[i];
            best = i;
        }
    }
    // Every simulation failed: fall back rather than guess.
    if (best == n) return defaultAction(state, legal);
    const Intent& chosen = plans[best][0];
    plan_.assign(plans[best].begin() + 1, plans[best].end());

    // Each option's score at each step of the chosen plan: the best plan
    // that agrees with the chosen one before that step and takes the option
    // there. Step 0 is this decision.
    plan_scores_.clear();
    for (size_t d = 0; d < plans[best].size(); ++d) {
        std::vector<std::pair<Intent, double>> options;
        for (size_t i = 0; i < n; ++i) {
            if (std::isnan(mean[i]) || plans[i].size() <= d ||
                !std::equal(plans[i].begin(), plans[i].begin() + d, plans[best].begin()))
                continue;
            auto it = std::find_if(options.begin(), options.end(),
                                   [&](const auto& o) { return o.first == plans[i][d]; });
            if (it == options.end()) options.emplace_back(plans[i][d], mean[i]);
            else it->second = std::max(it->second, mean[i]);
        }
        if (d == 0) {
            if (observer_) {
                std::vector<double> scores(legal.size(), std::nan(""));
                for (const auto& [option, score] : options) {
                    const int j = indexOf(legal, option);
                    if (j >= 0) scores[j] = score;
                }
                observer_(state, legal, scores);
            }
        } else {
            plan_scores_.push_back(std::move(options));
        }
    }
    if (chosen.type == IntentType::EndTurn) {
        ++stats_.lookahead_end_turns;
        countUnspentEnergy(state, legal, me);
    }
    return chosen;
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

bool HeuristicAgent::isFollowup(const GameState& state, const std::vector<Intent>& legal,
                                PlayerId me) const {
    if (legal.size() < 2 || legal[0].player != me) return false;
    return searchKind(state, legal) == SearchKind::None;
}

void HeuristicAgent::expandPlans(const GameState& guess, uint64_t engine_seed, PlayerId me,
                                 std::vector<Intent> prefix,
                                 std::vector<std::vector<Intent>>& out) {
    // Play the prefix as simulate does, then look at the next decision.
    std::vector<Intent> options;
    try {
        EventBus bus;
        GameEngine engine(card_db_, bus, card_registry_);
        StepResult step = engine.resumeFromSnapshot(guess, engine_seed);
        const int last_turn = lastTurn(guess);
        const int root =step.kind == StepKind::NeedDecision ? indexOf(step.legal, prefix[0]) : -1;
        if (root >= 0) {
            step = engine.applyChoice(root);
            size_t next = 1;
            for (int s = 0; s < config_.max_rollout_steps && step.kind == StepKind::NeedDecision &&
                            engine.state().turn.turn_number <= last_turn;
                 ++s) {
                if (isFollowup(engine.state(), step.legal, me)) {
                    if (next == prefix.size()) {
                        options = step.legal;  // the decision to branch on
                        break;
                    }
                    const int i = indexOf(step.legal, prefix[next++]);
                    if (i < 0) break;
                    step = engine.applyChoice(i);
                    continue;
                }
                // A fresh search point of mine: the action is over.
                if (step.legal[0].player == me && step.legal.size() >= 2) break;
                const int pick = defaultIndex(engine.state(), step.legal, config_.policy_rollouts);
                step = engine.applyChoice(pick < 0 ? 0 : pick);
            }
        }
    } catch (const std::exception&) {
        options.clear();
    }

    const int depth = static_cast<int>(prefix.size()) - 1;
    if (options.empty() || depth >= config_.plan_followups ||
        static_cast<int>(out.size()) >= config_.max_plans) {
        out.push_back(std::move(prefix));
        return;
    }
    const size_t width = std::min(options.size(), static_cast<size_t>(config_.max_followup_options));
    for (size_t k = 0; k < width && static_cast<int>(out.size()) < config_.max_plans; ++k) {
        std::vector<Intent> longer = prefix;
        longer.push_back(options[k]);
        expandPlans(guess, engine_seed, me, std::move(longer), out);
    }
}

std::optional<double> HeuristicAgent::simulate(const GameState& guess,
                                               const std::vector<Intent>& plan,
                                               PlayerId me,
                                               uint64_t engine_seed) {
    try {
        EventBus bus;
        GameEngine engine(card_db_, bus, card_registry_);
        StepResult step = engine.resumeFromSnapshot(guess, engine_seed);

        const int index = step.kind == StepKind::NeedDecision ? indexOf(step.legal, plan[0]) : -1;
        if (index < 0) {
            ++stats_.failed_simulations;
            return std::nullopt;
        }

        const int last_turn = lastTurn(guess);
        step = engine.applyChoice(index);
        size_t next = 1;  // plan step to play at the next follow-up decision
        int s = 0;
        for (; s < config_.max_rollout_steps &&
               step.kind == StepKind::NeedDecision &&
               engine.state().turn.turn_number <= last_turn;
             ++s) {
            if (next < plan.size() && isFollowup(engine.state(), step.legal, me)) {
                const int i = indexOf(step.legal, plan[next]);
                next = i >= 0 ? next + 1 : plan.size();  // a guess without it: play on as scripted
                if (i >= 0) {
                    step = engine.applyChoice(i);
                    continue;
                }
            }
            const int pick = defaultIndex(engine.state(), step.legal, config_.policy_rollouts);
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
