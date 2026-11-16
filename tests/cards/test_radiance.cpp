/// @file test_radiance.cpp
/// Radiance cards (ids 956..1099, by collector number). The Calm / Mind
/// cards implemented so far.

#include "tests/cards/card_test_fixture.h"

#include "cards/card.h"
#include "cards/card_helpers.h"
#include "cards/card_registry.h"
#include "core/events.h"
#include "core/game_state.h"
#include "engine/effect_executor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace riftbound::test {
namespace {

constexpr CardDefId kConsultTheHeavens   = 977;
constexpr CardDefId kIridescentAvian     = 979;
constexpr CardDefId kKiBurst             = 980;
constexpr CardDefId kLifeFromStone       = 981;
constexpr CardDefId kCommandAttack       = 983;
constexpr CardDefId kHunkerDown          = 984;
constexpr CardDefId kInvigoratingBloom   = 985;
constexpr CardDefId kHighNote            = 996;
constexpr CardDefId kParallelConvergence = 999;
constexpr CardDefId kImprovisedProjectile = 1002;
constexpr CardDefId kRiptideMage         = 1004;
constexpr CardDefId kEkkoIngenious       = 1007;
constexpr CardDefId kBrutalizer          = 365;   // a 2-cost gear
constexpr CardDefId kSteraksGage         = 379;   // a 3-cost gear

/// Picks the option that names `id`; otherwise the first option.
std::function<Intent(const std::vector<Intent>&)> pickIds(std::vector<GameObjectId> ids) {
    auto order = std::make_shared<std::vector<GameObjectId>>(std::move(ids));
    auto next = std::make_shared<size_t>(0);
    return [order, next](const std::vector<Intent>& legal) {
        if (*next < order->size()) {
            const GameObjectId want = (*order)[(*next)++];
            for (const auto& i : legal)
                if (!i.chosen_objects.empty() && i.chosen_objects.front() == want) return i;
        }
        return legal.empty() ? Intent{} : legal.front();
    };
}

class Radiance : public CardTestFixture {
protected:
    GameObjectId addGear(PlayerId owner, CardDefId def_id) {
        auto id = addUnit(owner, def_id);
        auto& obj = state.getObject(id);
        obj.card_type = CardType::Gear;
        obj.base_might = obj.current_might = 0;
        return id;
    }
    GameObjectId exhausted(GameObjectId id) {
        state.getObject(id).is_exhausted = true;
        return id;
    }
    /// Drive an activated ability through the choice loop (as driveResumable
    /// does for spells).
    void driveActivation(CardDefId def_id, PlayerId controller, GameObjectId source,
                         std::function<Intent(const std::vector<Intent>&)> picker,
                         EffectExecutor& exec) {
        ChainItem ri;
        ri.id = state.chain.allocateId();
        ri.controller = controller;
        ri.is_ability = true;
        ri.is_activated_ability = true;
        ri.source = source;
        ri.card_def_id = def_id;
        state.chain.resuming = ri;
        Card* card = card_registry.get(def_id);
        ASSERT_NE(card, nullptr);
        CardContext ctx{state, events, exec, controller, source};
        card->onActivate(ctx, 0, {});
        while (exec.hasPendingChoice()) {
            auto pending = exec.consumePendingChoice();
            exec.recordChoice(picker(pending.legal));
            card->onActivate(ctx, 0, {});
        }
        state.chain.resuming.reset();
    }
};

TEST_F(Radiance, CardsAreRegisteredWithTheirRadianceIds) {
    for (CardDefId id : {kConsultTheHeavens, kIridescentAvian, kKiBurst, kLifeFromStone,
                         kCommandAttack, kHunkerDown, kInvigoratingBloom, kHighNote,
                         kParallelConvergence, kImprovisedProjectile, kRiptideMage,
                         kEkkoIngenious}) {
        const Card* c = card_registry.get(id);
        ASSERT_NE(c, nullptr) << id;
        EXPECT_EQ(c->def().id, id);
        EXPECT_EQ(c->def().set_code, "RAD");
        EXPECT_EQ(card_db.get(id).name, c->def().name);
    }
    EXPECT_EQ(card_db.get(kKiBurst).name, "Ki Burst");
}

// ── 980 Ki Burst ─────────────────────────────────────────────────────────

TEST_F(Radiance, KiBurst_NeedsAReadyFriendlyUnitToPay) {
    const Card* c = card_registry.get(kKiBurst);
    exhausted(addUnit(P1, kInvalidId, 2));
    addUnit(P2, kInvalidId, 3, 0);
    EXPECT_FALSE(c->canBePlayed(state, P1));
    addUnit(P1, kInvalidId, 2);
    EXPECT_TRUE(c->canBePlayed(state, P1));
}

TEST_F(Radiance, KiBurst_ExhaustsTheChosenFriendlyAndStunsTheChosenUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto mine = addUnit(P1, kInvalidId, 2);
    auto enemy = addUnit(P2, kInvalidId, 4, 0);
    auto spell = addToHand(P1, kKiBurst);
    driveResumable(kKiBurst, P1, spell, pickIds({mine, enemy}), exec);
    EXPECT_TRUE(state.getObject(mine).is_exhausted);
    EXPECT_TRUE(state.getObject(enemy).is_stunned);
    EXPECT_FALSE(state.getObject(mine).is_stunned);
}

// ── 981 Life from Stone ──────────────────────────────────────────────────

TEST_F(Radiance, LifeFromStone_ExhaustsAFriendlyUnitAndDraws) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* c = card_registry.get(kLifeFromStone);
    EXPECT_FALSE(c->canBePlayed(state, P1));
    auto mine = addUnit(P1, kInvalidId, 2);
    EXPECT_TRUE(c->canBePlayed(state, P1));
    addToDeck(P1, kBrutalizer);
    auto spell = addToHand(P1, kLifeFromStone);
    const size_t hand = state.player(P1).hand.size();
    driveResumable(kLifeFromStone, P1, spell, pickIds({mine}), exec);
    EXPECT_TRUE(state.getObject(mine).is_exhausted);
    EXPECT_EQ(state.player(P1).hand.size(), hand + 1);
}

// ── 984 Hunker Down ──────────────────────────────────────────────────────

TEST_F(Radiance, HunkerDown_ReadiesAUnitThatCantMoveThisTurnAndDraws) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto mine = exhausted(addUnit(P1, kInvalidId, 3, 0));
    addToDeck(P1, kBrutalizer);
    auto spell = addToHand(P1, kHunkerDown);
    const size_t hand = state.player(P1).hand.size();
    driveResumable(kHunkerDown, P1, spell, pickIds({mine}), exec);
    EXPECT_FALSE(state.getObject(mine).is_exhausted);
    EXPECT_EQ(state.getObject(mine).cant_move_on_turn, state.turn.turn_number);
    EXPECT_EQ(state.player(P1).hand.size(), hand + 1);
}

// ── 977 Consult the Heavens ──────────────────────────────────────────────

TEST_F(Radiance, ConsultTheHeavens_GivesShieldTwo) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto mine = addUnit(P1, kInvalidId, 3, 0);
    addToDeck(P1, kBrutalizer);
    auto spell = addToHand(P1, kConsultTheHeavens);
    driveResumable(kConsultTheHeavens, P1, spell, pickIds({mine}), exec);
    EXPECT_TRUE(state.getObject(mine).hasKeyword(Keyword::Shield));
}

// ── 983 Command: Attack ──────────────────────────────────────────────────

TEST_F(Radiance, CommandAttack_DrawsAGearFromTheTopFiveAndRecyclesTheRest) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto deep = addToDeck(P1, kBrutalizer);        // 6th from the top: not looked at
    std::vector<GameObjectId> top;
    for (int i = 0; i < 4; ++i) top.push_back(addToDeck(P1, kLifeFromStone));
    auto gear = addToDeck(P1, kSteraksGage);        // the top card
    auto spell = addToHand(P1, kCommandAttack);
    driveResumable(kCommandAttack, P1, spell, [&](const std::vector<Intent>& legal) {
        for (const auto& i : legal)   // "you may reveal a gear": yes
            if (i.chosen_objects.empty() && i.chosen_value == 1) return i;
        for (const auto& i : legal)
            if (!i.chosen_objects.empty() && i.chosen_objects.front() == gear) return i;
        return legal.front();
    }, exec);

    const auto& ps = state.player(P1);
    EXPECT_NE(std::find(ps.hand.begin(), ps.hand.end(), gear), ps.hand.end());
    EXPECT_EQ(std::find(ps.hand.begin(), ps.hand.end(), deep), ps.hand.end());
    ASSERT_EQ(ps.main_deck.size(), 5u);
    EXPECT_EQ(ps.main_deck.back(), deep);           // the unseen card is on top now
}

// ── 999 Parallel Convergence ─────────────────────────────────────────────

TEST_F(Radiance, ParallelConvergence_ReturnsAFriendlyUnitAndDealsFive) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* c = card_registry.get(kParallelConvergence);
    auto enemy = addUnit(P2, kInvalidId, 5, 0);
    EXPECT_FALSE(c->canBePlayed(state, P1));
    auto mine = addUnit(P1, kInvalidId, 2);
    EXPECT_TRUE(c->canBePlayed(state, P1));
    EXPECT_TRUE(c->hasLegalTargets(state, P1));
    auto spell = addToHand(P1, kParallelConvergence);
    driveResumable(kParallelConvergence, P1, spell, pickIds({mine, enemy}), exec);
    const auto& hand = state.player(P1).hand;
    EXPECT_NE(std::find(hand.begin(), hand.end(), mine), hand.end());
    EXPECT_TRUE(!state.objectExists(enemy) || !state.getObject(enemy).location.has_value());
}

// ── 996 High Note ────────────────────────────────────────────────────────

TEST_F(Radiance, HighNote_MinusOneMightPerExhaustedFriendlyUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    exhausted(addUnit(P1, kInvalidId, 2));
    exhausted(addUnit(P1, kInvalidId, 2, 0));
    addUnit(P1, kInvalidId, 2);                     // ready: doesn't count
    auto enemy = addUnit(P2, kInvalidId, 4, 0);
    auto spell = addToHand(P1, kHighNote);
    driveResumable(kHighNote, P1, spell, pickIds({enemy}), exec);
    EXPECT_EQ(state.getObject(enemy).current_might, 2);
}

// ── 1002 Improvised Projectile ───────────────────────────────────────────

TEST_F(Radiance, ImprovisedProjectile_DealsTheGearsEnergyCost) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* c = card_registry.get(kImprovisedProjectile);
    auto enemy = addUnit(P2, kInvalidId, 4, 0);
    EXPECT_FALSE(c->hasLegalTargets(state, P1));    // no friendly gear
    auto gear = addGear(P1, kSteraksGage);
    EXPECT_TRUE(c->hasLegalTargets(state, P1));
    auto spell = addToHand(P1, kImprovisedProjectile);
    driveResumable(kImprovisedProjectile, P1, spell, pickIds({gear, enemy}), exec);
    ASSERT_TRUE(state.objectExists(enemy));
    EXPECT_EQ(state.getObject(enemy).damage_marked, 3);
}

// ── 979 Iridescent Avian ─────────────────────────────────────────────────

TEST_F(Radiance, IridescentAvian_BuffsThenGivesTwoToABuffedUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto avian = addUnit(P1, kIridescentAvian);
    driveActivation(kIridescentAvian, P1, avian, pickIds({avian}), exec);
    EXPECT_EQ(state.getObject(avian).buff_count, 1);
    const int might = state.getObject(avian).current_might;
    driveActivation(kIridescentAvian, P1, avian, pickIds({avian}), exec);
    EXPECT_EQ(state.getObject(avian).buff_count, 1);
    EXPECT_EQ(state.getObject(avian).current_might, might + 2);
}

// ── 985 Invigorating Bloom ───────────────────────────────────────────────

TEST_F(Radiance, InvigoratingBloom_DrawNeedsAReadyMightyUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* c = card_registry.get(kInvigoratingBloom);
    auto bloom = addGear(P1, kInvigoratingBloom);
    addUnit(P1, kInvalidId, 4);
    EXPECT_FALSE(c->canActivateAbilityAt(state, P1, bloom, 0));
    auto mighty = addUnit(P1, kInvalidId, 5);
    EXPECT_TRUE(c->canActivateAbilityAt(state, P1, bloom, 0));
    addToDeck(P1, kBrutalizer);
    const size_t hand = state.player(P1).hand.size();
    driveActivation(kInvigoratingBloom, P1, bloom, pickIds({mighty}), exec);
    EXPECT_TRUE(state.getObject(mighty).is_exhausted);
    EXPECT_EQ(state.player(P1).hand.size(), hand + 1);
}

TEST_F(Radiance, InvigoratingBloom_PlayGivesAFriendlyUnitThree) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto bloom = addGear(P1, kInvigoratingBloom);
    auto mine = addUnit(P1, kInvalidId, 2);
    driveResumableTrigger(kInvigoratingBloom, P1, bloom, pickIds({mine}), exec,
                          TriggerType::WhenYouPlayThis);
    EXPECT_EQ(state.getObject(mine).current_might, 5);
}

// ── 1004 Riptide Mage ────────────────────────────────────────────────────

TEST_F(Radiance, RiptideMage_PlayGivesAnEnemyMinusThree) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto mage = addUnit(P1, kRiptideMage);
    auto enemy = addUnit(P2, kInvalidId, 5, 0);
    driveResumableTrigger(kRiptideMage, P1, mage, pickIds({enemy}), exec,
                          TriggerType::WhenYouPlayMe);
    EXPECT_EQ(state.getObject(enemy).current_might, 2);
    EXPECT_TRUE(card_db.get(kRiptideMage).keywords.has(Keyword::Accelerate));
}

// ── 1007 Ekko, Ingenious ─────────────────────────────────────────────────

TEST_F(Radiance, EkkoIngenious_NeedsAFriendlyUnitAndReturnsOneOtherThanItself) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* c = card_registry.get(kEkkoIngenious);
    EXPECT_FALSE(c->canBePlayed(state, P1));
    auto mine = addUnit(P1, kInvalidId, 2, 0);
    EXPECT_TRUE(c->canBePlayed(state, P1));
    auto ekko = addUnit(P1, kEkkoIngenious);
    driveResumableTrigger(kEkkoIngenious, P1, ekko, pickIds({ekko, mine}), exec,
                          TriggerType::WhenYouPlayMe);
    const auto& hand = state.player(P1).hand;
    EXPECT_NE(std::find(hand.begin(), hand.end(), mine), hand.end());
    EXPECT_TRUE(state.getObject(ekko).location.has_value());
    EXPECT_TRUE(card_db.get(kEkkoIngenious).keywords.has(Keyword::Ganking));
}

}  // namespace
}  // namespace riftbound::test
