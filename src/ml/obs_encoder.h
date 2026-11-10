#pragma once
/// @file obs_encoder.h
/// Structured observation for the neural policy / value model (agents/nn_model).
///
/// The gradient-boosted models read a bag of card ids per zone plus a few
/// totals (ml/value_features). This encoding keeps what they cannot see:
///
///   globals    kObsGlobalDim floats: points, turn, phase, ready runes per
///              domain, the floating pool, zone sizes, ...
///   entities   one per object the player can see and reason about, each with
///              kObsEntityDim small integers:
///                units and gear on the board (Might, damage, exhausted,
///                buffs, keywords, where they are, what gear is attached),
///                the player's own hand and facedown cards,
///                the battlefields (who holds them, Might on each side,
///                scored this turn, a Showdown or Combat there),
///                both legends and champion-zone cards, the chain's items.
///   bags       card ids without state: the player's deck (they know what is
///              left in it, not the order), both trashes.
///
/// Encoded from one player's point of view with only what they can see: the
/// opponent's hand, deck and facedown cards appear as counts.
///
/// A legal action (encodeActions) is the 96 action numbers of
/// ml/policy_features plus references into the entities: the acting card,
/// the chosen objects, the units moved and the destination battlefield.
///
/// The same structs are what the recorder writes (ml/obs_io.h) and what the
/// model reads, so training and play cannot drift apart. Changing a layout
/// invalidates trained models: bump kObsVersion.

#include "core/card_db.h"
#include "core/game_state.h"
#include "core/intent.h"
#include "ml/policy_features.h"

#include <cstdint>
#include <string>
#include <vector>

namespace riftbound::ml {

inline constexpr int kObsVersion = 1;
inline constexpr int kObsGlobalDim = 80;
inline constexpr int kObsEntityDim = 112;
inline constexpr int kObsActionDim = kPolicyActionGlobalDim;
inline constexpr int kObsMaxChosen = 4;
inline constexpr int kObsMaxMoved = 6;
inline constexpr int kObsMaxBattlefields = 4;
/// Card ids 1..kCardVocabSize; 0 = no card or one outside the vocabulary
/// (its printed numbers are still in the entity's features).
inline constexpr int kObsCardVocab = kCardVocabSize + 1;

enum class ObsGroup : uint8_t {
    MyUnit = 0, OppUnit, MyGear, OppGear, MyHand, MyFacedown, Battlefield,
    MyLegend, OppLegend, MyChampion, OppChampion, MyChain, OppChain,
    Count,
};
inline constexpr int kObsGroupSlots = 16;  // one-hot width (room to grow)

enum class ObsBag : uint8_t { MyDeck = 0, MyTrash, OppTrash, Count };

/// Pools an entity is summed / maxed into. Battlefield k has two: the
/// player's units there (kObsPoolBattlefield + 2k) and the opponent's (+1).
inline constexpr int kObsPoolMyBase = 0;
inline constexpr int kObsPoolOppBase = 1;
inline constexpr int kObsPoolHand = 2;
inline constexpr int kObsPoolFacedown = 3;
inline constexpr int kObsPoolChain = 4;
inline constexpr int kObsPoolBattlefield = 5;
inline constexpr int kObsPoolCount = kObsPoolBattlefield + 2 * kObsMaxBattlefields;
inline constexpr uint8_t kObsNoPool = 255;

/// Entity feature indices. [0, 32) state of an object on the board,
/// [32, 80) the card's printed numbers and current keywords, [80, 112) a
/// battlefield's state.
namespace ent {
enum : int {
    Might = 0, BaseMight, Damage, HealthLeft, Exhausted, Stunned, Buffs, TempMight,
    Attachments, AttachmentMight, IsToken, Empowered, Attached, Attacker, Defender,
    MovesThisTurn, Assault, Shield, Deflect, Untargetable, DamagePrevented,
    ControlReverts, CantMove, Hidden, Stolen, AuraMight, DamageDoubled, HiddenCanReact,
    TextInactive, TargetedByOppChain, TargetedByMyChain, ChainDepth,
    EnergyCost = 32, PowerCost, PrintedMight, GearMightBonus,
    TypeFirst = 36,      // 6: Unit, Spell, Gear, Rune, Battlefield, Legend
    SuperFirst = 42,     // 4: None, Champion, Signature, Token
    DomainFirst = 46,    // 6
    KeywordFirst = 52,   // 25
    InVocab = 77, ChainIsAbility = 78,
    BfMine = 80, BfTheirs, BfMyMight, BfOppMight, BfMyUnits, BfOppUnits, BfContested,
    BfContestedByMe, BfContestedByOpp, BfContributedByMe, BfScoredByMe, BfScoredByOpp,
    BfShowdown, BfCombat, BfAttackerMe, BfAttackerOpp, BfMyFacedown, BfOppFacedown,
    BfIsToken, BfConqueredByMe, BfConqueredByOpp, BfCannotScoreYet, BfBlocksMoveToBase,
    BfBlocksUnitPlay, BfAcceptsAnyInbound, BfMyReadyUnits, BfOppReadyUnits, BfFocusHere,
};
}

/// Global feature indices.
namespace glob {
enum : int {
    MyScore = 0, OppScore, VictoryScore, MyToWin, OppToWin, Turn, IsMyTurn, IWentFirst,
    MyTurnsTaken, OppTurnsTaken, PhaseMain, Showdown, Closed, ChainSize, AwaitingMain,
    IHoldPriority, IHoldFocus,
    MyRunes = 17, MyRunesReady, MyRuneDeck,
    MyReadyDomainFirst = 20,   // 6
    OppRunes = 26, OppRunesReady, OppRuneDeck,
    MyPoolEnergy = 29, MyPoolUniversal,
    MyPoolDomainFirst = 31,    // 6
    MyRestrictedEnergy = 37, MyRestrictedPower, OppPoolEnergy, OppPoolPower,
    MyHand = 41, OppHand, MyDeck, OppDeck, MyTrash, OppTrash, MyBanished, OppBanished,
    MyXp = 49, OppXp, MyCardsPlayed, OppCardsPlayed, Battlefields, MyHeld, OppHeld,
    MyScoredThisTurn, OppScoredThisTurn, HandEval, MyRuneEnergy, OppRuneEnergy,
    MyTrashReplayable, OppTrashReplayable, MyBoardMight, OppBoardMight, MyUnits, OppUnits,
    MyReadyUnits, OppReadyUnits, MyBurnedOut, OppBurnedOut, CantPlayCards,
    CostEnergyLeft, CostPowerLeft, UnitDiedThisTurn, MyPowerSpent,
};
}

struct ObsEntity {
    uint16_t card = 0;
    uint8_t group = 0;
    uint8_t place = 0;             // 0 none, 1 my base, 2 opponent's base, 3 + k battlefield k
    uint8_t pool = kObsNoPool;
    int16_t host = -1;             // gear: index of the entity it is attached to
    int8_t f[kObsEntityDim] = {};
    GameObjectId object = kInvalidId;  // not recorded: what actions refer to
};

struct Obs {
    float g[kObsGlobalDim] = {};
    std::vector<ObsEntity> entities;
    std::vector<std::pair<uint8_t, uint16_t>> bags;  // (ObsBag, card id)

    /// Index of the entity for `object`, or -1.
    int find(GameObjectId object) const {
        if (object == kInvalidId) return -1;
        for (size_t i = 0; i < entities.size(); ++i)
            if (entities[i].object == object) return static_cast<int>(i);
        return -1;
    }
};

struct ObsAction {
    int16_t a[kObsActionDim] = {};
    int16_t acting = -1;           // entity index of the card played / ability source
    uint16_t acting_card = 0;
    int16_t dest = -1;             // battlefield slot moved or played to
    int16_t chosen[kObsMaxChosen] = {-1, -1, -1, -1};
    uint16_t chosen_card[kObsMaxChosen] = {};
    int16_t moved[kObsMaxMoved] = {-1, -1, -1, -1, -1, -1};
    uint8_t n_chosen = 0;
    uint8_t n_moved = 0;
};

/// Encode `state` as `me` sees it (out is overwritten). Entities come in a
/// fixed order (group, place, object id), so the encoding of equal states is
/// equal whatever order the objects are stored in.
void encodeObs(const GameState& state, PlayerId me, const CardDB& card_db, Obs& out);

/// Encode every legal action against `obs`, which must be encodeObs of the
/// same state for legal[0].player. `kind` is ml::policyDecisionKind and
/// `scripted_index` ScriptedAgent's pick (-1 if unknown).
void encodeActions(const GameState& state, const CardDB& card_db,
                   const std::vector<Intent>& legal, const Obs& obs, const char* kind,
                   int scripted_index, std::vector<ObsAction>& out);

/// Index of a decision kind (ml::policyDecisionKind) in obsKindNames, or the
/// last one ("other").
int obsKindIndex(const char* kind);
const std::vector<std::string>& obsKindNames();
std::vector<std::string> obsGlobalNames();
std::vector<std::string> obsEntityNames();

} // namespace riftbound::ml
