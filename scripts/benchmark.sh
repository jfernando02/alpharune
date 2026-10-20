#!/usr/bin/env bash
# Win rate of one agent against others, with each seat played equally.
#
#   scripts/benchmark.sh [--bin PATH] [--games N] [--threads T] [--seed S]
#                        [--agent SPEC] [--opponents "SPEC SPEC ..."]
#
# For every opponent and every deck pair it runs N games with the agent as
# Player 1 and N with it as Player 2 (deck order swapped too, so the agent
# plays each deck from each seat), on fixed seeds. Prints wins / games and a
# 95% Wilson interval per opponent. Draws count as non-wins.
#
# Run from the repo root (deck paths are relative to it).

set -euo pipefail

BIN=./build/riftbound
GAMES=50
THREADS=0
SEED=1
AGENT=heuristic
OPPONENTS="random scripted"
DECK_PAIRS=(
    "decks/leblanc_test.txt decks/vex_test_deck.txt"
    "decks/darius_test.txt decks/ahri_test.txt"
    "decks/jax_test.txt decks/garen_test.txt"
    "decks/miss_fortune_test.txt decks/viktor_test.txt"
)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --games) GAMES="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --agent) AGENT="$2"; shift 2 ;;
        --opponents) OPPONENTS="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# wins for one seat from a batch run's "P1 wins:" / "P2 wins:" lines
run() {  # deck1 deck2 agent1 agent2 seat_of_agent(1|2)
    "$BIN" --deck1 "$1" --deck2 "$2" --agent1 "$3" --agent2 "$4" \
           --games "$GAMES" --threads "$THREADS" --seed "$SEED" 2>/dev/null |
        awk -v seat="P$5" '$1 == seat && $2 == "wins:" { print $3 }'
}

printf '%-24s %-24s %9s %8s  %s\n' agent opponent wins games "win rate (95% CI)"
for opp in $OPPONENTS; do
    wins=0
    games=0
    for pair in "${DECK_PAIRS[@]}"; do
        read -r d1 d2 <<<"$pair"
        [[ -f "$d1" && -f "$d2" ]] || { echo "skipping missing deck pair: $pair" >&2; continue; }
        for order in "$d1 $d2" "$d2 $d1"; do
            read -r a b <<<"$order"
            w1=$(run "$a" "$b" "$AGENT" "$opp" 1)
            w2=$(run "$a" "$b" "$opp" "$AGENT" 2)
            wins=$((wins + ${w1:-0} + ${w2:-0}))
            games=$((games + 2 * GAMES))
        done
    done
    awk -v w="$wins" -v n="$games" -v a="$AGENT" -v o="$opp" 'BEGIN {
        if (n == 0) { printf "%-24s %-24s %9s %8s\n", a, o, "-", 0; exit }
        z = 1.96; p = w / n
        c = (p + z*z/(2*n)) / (1 + z*z/n)
        h = z * sqrt(p*(1-p)/n + z*z/(4*n*n)) / (1 + z*z/n)
        printf "%-24s %-24s %9d %8d  %.3f (%.3f-%.3f)\n", a, o, w, n, p, c-h, c+h
    }'
done
