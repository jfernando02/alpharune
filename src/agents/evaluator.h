#pragma once
/// @file evaluator.h
/// Hand-written position score for the look-ahead agent.
///
/// The score reads only what a player at the table can see: points, who
/// holds each battlefield, the units on the board, the sizes of the hands,
/// and the trash cards each player may play again. It never looks at which cards are in a hand or a deck, so it is
/// safe to call on a determinized state without leaking anything.
///
/// Every term is "mine minus theirs", so evaluate(s, P1) == -evaluate(s, P2).

#include "core/card_db.h"
#include "core/game_state.h"

#include <string>

namespace riftbound {

struct EvalWeights {
    double points = 100.0;        // per point of score difference
    double controlled_bf = 60.0;  // per battlefield held (worth ~ a point next turn)
    double contested_might = 5.0; // per point of Might lead at a battlefield, capped
    double board_might = 3.0;     // per point of Might on the board anywhere
    // per card in hand. Kept below what a card is worth once played (a
    // 1-Might unit is 3): at 8 the look-ahead preferred ending its turn to
    // casting anything that did not take a battlefield that turn, and lost
    // to ScriptedAgent; at 2 it beats it ~74% (leblanc / vex, both seats).
    double hand = 2.0;
    // per card its owner may currently play from trash (Flow, Death from
    // Below). Equal to `hand` because such a card plays like one in hand
    // (CR 829.1.b.2); tuning decides whether its different cost matters.
    double trash_replayable = 2.0;
    double runes = 0.0;           // per rune in base (flat; superseded by rune_energy)
    // per point of Energy the player's runes are projected to give over the
    // next `rune_horizon` turns: runes in base grow by 2 a turn (channel)
    // up to all the runes they own. A rune recycled for power early is gone
    // for ~5 turns until the rune deck runs dry; late it is back next turn.
    // The flat `runes` term prices both the same. Against ScriptedAgent
    // (4 deck pairs, 400 games each): flat runes=6 77.0%, rune_energy=1
    // 78.7%, rune_energy=2 79.2% — within noise, kept for the game logic.
    double rune_energy = 1.0;
    int rune_horizon = 6;
    int might_cap = 5;            // a Might lead past this at one battlefield earns nothing
};

/// Set the weight called `name` (the field name, e.g. "hand"). Returns false
/// for an unknown name. Lets the CLI and tuning scripts change weights
/// without a rebuild.
bool setWeight(EvalWeights& weights, const std::string& name, double value);

/// Score of a finished game for the winner (the loser gets the negation).
inline constexpr double kWinScore = 1e9;

/// Score `state` from `me`'s point of view. Higher is better for `me`.
double evaluate(const GameState& state, PlayerId me, const EvalWeights& weights = {});

/// Distinct cards in `player`'s trash that a standing grant lets them play.
int trashReplayable(const GameState& state, PlayerId player);

/// Energy `player`'s runes give over the next `horizon` turns if they
/// channel 2 a turn into base, up to every rune they own (base + rune deck).
int projectedRuneEnergy(const GameState& state, PlayerId player, int horizon);

/// A learned position evaluator. When an agent is given one it replaces
/// `evaluate`: the agent scores a position by `me`'s chance of winning.
/// Implementations must read only what `me` can see.
class PositionModel {
public:
    virtual ~PositionModel() = default;
    /// Probability in [0, 1] that `me` wins from `state` (not game over).
    virtual double winProbability(const GameState& state, PlayerId me) const = 0;
    /// The same for callers that have the card database, which a model whose
    /// encoding reads printed card data needs (agents/nn_model).
    virtual double winProbability(const GameState& state, PlayerId me, const CardDB&) const {
        return winProbability(state, me);
    }
};

} // namespace riftbound
