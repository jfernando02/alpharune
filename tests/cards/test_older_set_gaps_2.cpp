/// @file test_older_set_gaps_2.cpp
/// The second batch of older-set cards that used to be approximated: buff
/// spending (Fae Dragon, The Boss), readying (Maduli), protection auras (Alpha
/// Wildclaw, Master Yi), turn trackers (Stand United, Prepared Neophyte),
/// repeating delayed abilities (Rally the Troops), trash triggers (Super Mega
/// Death Rocket), cost changes (Commander Ledros, Master Yi, Swift Scout),
/// enter-ready grants (Magma Wurm, Wuju Master) and Void Assault's attacker.

#include "tests/cards/card_test_fixture.h"

#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/chain_manager.h"
#include "engine/effect_executor.h"
#include "engine/trigger_manager.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace riftbound::test {
namespace {

constexpr CardDefId kMagmaWurm        = 11;
constexpr CardDefId kStandUnited      = 53;
constexpr CardDefId kCommanderLedros  = 231;
constexpr CardDefId kSuperMegaRocket  = 250;
constexpr CardDefId kSwiftScout       = 259;
constexpr CardDefId kTheBoss          = 264;
constexpr CardDefId kFaeDragon        = 423;
constexpr CardDefId kRallyTheTroops   = 487;
constexpr CardDefId kPreparedNeophyte = 563;
constexpr CardDefId kAlphaWildclaw    = 619;
constexpr CardDefId kMasterYi         = 621;
constexpr CardDefId kMaduli           = 706;
constexpr CardDefId kVoidAssault      = 758;
constexpr CardDefId kWujuMaster       = 784;
constexpr CardDefId kFiller           = 144;   // Spoils of War: any card

Intent pickObj(const std::vector<Intent>& legal, GameObjectId id) {
    for (const auto& i : legal)
        if (std::find(i.chosen_objects.begin(), i.chosen_objects.end(), id) != i.chosen_objects.end())
            return i;
    ADD_FAILURE() << "object " << id << " not offered";
    return legal.front();
}
Intent pickValue(const std::vector<Intent>& legal, int v) {
    for (const auto& i : legal)
        if (i.chosen_value.has_value() && *i.chosen_value == v) return i;
    ADD_FAILURE() << "value " << v << " not offered";
    return legal.front();
}

class OlderSetGaps2 : public CardTestFixture {
protected:
    int itemsFrom(CardDefId id) {
        int n = 0;
        for (const auto& it : state.chain.items) if (it.card_def_id == id) ++n;
        return n;
    }
    GameObjectId addToTrash(PlayerId owner, CardDefId def_id) {
        const auto id = addToHand(owner, def_id);
        auto& h = state.player(owner).hand;
        h.erase(std::find(h.begin(), h.end(), id));
        state.getObject(id).zone = ZoneType::Trash;
        state.player(owner).trash.push_back(id);
        return id;
    }
    Card* card(CardDefId id) { return card_registry.get(id); }
};

TEST_F(OlderSetGaps2, FaeDragon_SpendingABuffTriggersIt) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    addUnit(P1, kFaeDragon);
    const auto u = addUnit(P1, kInvalidId, 2);
    exec.buffUnit(u);
    ASSERT_EQ(state.getObject(u).buff_count, 1);
    exec.spendBuff(u);
    EXPECT_EQ(state.getObject(u).buff_count, 0);
    EXPECT_EQ(itemsFrom(kFaeDragon), 1);
    exec.spendBuff(u);
    EXPECT_EQ(itemsFrom(kFaeDragon), 1) << "no buff to spend";
}

TEST_F(OlderSetGaps2, Maduli_CantBeReadied) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto maduli = addUnit(P1, kMaduli);
    const auto other = addUnit(P1, kInvalidId, 2);
    state.getObject(maduli).is_exhausted = true;
    state.getObject(other).is_exhausted = true;
    exec.readyObject(maduli);
    exec.readyObject(other);
    EXPECT_TRUE(state.getObject(maduli).is_exhausted);
    EXPECT_FALSE(state.getObject(other).is_exhausted);
}

TEST_F(OlderSetGaps2, AlphaWildclaw_ProtectsSmallerFriendlyUnitsHere) {
    const auto claw = addUnit(P1, kAlphaWildclaw, 5, 0);
    const auto small = addUnit(P1, kInvalidId, 2, 0);
    const auto big = addUnit(P1, kInvalidId, 7, 0);
    const auto away = addUnit(P1, kInvalidId, 2, 1);
    card(kAlphaWildclaw)->applyPassiveAura(state, P1);
    EXPECT_TRUE(state.getObject(small).untargetable_by_enemy);
    EXPECT_FALSE(state.getObject(big).untargetable_by_enemy);
    EXPECT_FALSE(state.getObject(away).untargetable_by_enemy);
    EXPECT_FALSE(state.getObject(claw).untargetable_by_enemy);
}

TEST_F(OlderSetGaps2, StandUnited_BuffsGiveMoreMightThisTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto u = addUnit(P1, kInvalidId, 2);
    const auto src = pushSpellOnChain(P1, kStandUnited);
    state.chain.items.clear();
    CardContext ctx{state, events, exec, P1, src};
    card(kStandUnited)->onResolve(ctx, {u});
    EXPECT_EQ(state.getObject(u).buff_count, 1);
    EXPECT_EQ(state.player(P1).extra_buff_might_this_turn, 1);
    state.player(P1).resetTurnTracking();
    EXPECT_EQ(state.player(P1).extra_buff_might_this_turn, 0);
}

TEST_F(OlderSetGaps2, PreparedNeophyte_AfterASpellOfFourOrMore) {
    const auto me = addUnit(P1, kPreparedNeophyte);
    state.player(P1).max_spell_spent_this_turn = 3;
    card(kPreparedNeophyte)->applyPassiveAura(state, P1);
    EXPECT_TRUE(state.getObject(me).aura_effects.empty());
    state.player(P1).max_spell_spent_this_turn = 4;
    card(kPreparedNeophyte)->applyPassiveAura(state, P1);
    ASSERT_EQ(state.getObject(me).aura_effects.size(), 1u);
    EXPECT_EQ(state.getObject(me).aura_effects[0].might_bonus, 4);
}

TEST_F(OlderSetGaps2, RallyTheTroops_BuffsEveryUnitPlayedThisTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    addToDeck(P1, kFiller);
    const auto src = pushSpellOnChain(P1, kRallyTheTroops);
    state.chain.items.clear();
    CardContext ctx{state, events, exec, P1, src};
    card(kRallyTheTroops)->onResolve(ctx, {});

    const auto a = addUnit(P1, kInvalidId, 2);
    const auto b = addUnit(P1, kInvalidId, 2);
    events.emit(CardPlayedEvent{a, P1, CardType::Unit, 1, 2});
    events.emit(CardPlayedEvent{b, P1, CardType::Unit, 2, 2});
    ASSERT_EQ(itemsFrom(kRallyTheTroops), 2) << "every unit, not just the first";
    for (const auto& it : state.chain.items) {
        driveResumableTrigger(kRallyTheTroops, P1, src, nullptr, exec,
                              TriggerType::WhenYouPlayAUnit, it.triggering_subject);
    }
    EXPECT_EQ(state.getObject(a).buff_count, 1);
    EXPECT_EQ(state.getObject(b).buff_count, 1);
}

TEST_F(OlderSetGaps2, SuperMegaDeathRocket_ReturnsFromTrashWhenYouConquer) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    const auto rocket = addToTrash(P1, kSuperMegaRocket);
    const auto fodder = addToHand(P1, kFiller);
    events.emit(ScoreEvent{P1, 0, ScoreMethod::Hold, 1});
    EXPECT_EQ(itemsFrom(kSuperMegaRocket), 0) << "a hold";
    events.emit(ScoreEvent{P1, 0, ScoreMethod::Conquer, 1});
    ASSERT_EQ(itemsFrom(kSuperMegaRocket), 1);
    driveResumableTrigger(kSuperMegaRocket, P1, rocket,
                          [](const std::vector<Intent>& l) { return l.back(); }, exec,
                          TriggerType::WhenYouConquerInTrash);
    EXPECT_TRUE(inHand(P1, rocket));
    EXPECT_TRUE(inTrash(P1, fodder)) << "discarded to pay";
}

TEST_F(OlderSetGaps2, TheBoss_PaysPowerToSaveABuffedUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto boss = addUnit(P1, kTheBoss);
    state.getObject(boss).card_type = CardType::Legend;
    state.getObject(boss).zone = ZoneType::LegendZone;
    const auto u = addUnit(P1, kInvalidId, 2, 0);
    exec.buffUnit(u);
    CardContext ctx{state, events, exec, P1, boss};
    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) { return pickObj(ch, u); });
    EXPECT_FALSE(card(kTheBoss)->applyReplacement(ctx, u)) << "no Body or Order power";

    const auto rune = addRune(P1, Domain::Order);
    exec.setAgentQuery([](PlayerId, const std::vector<Intent>& ch) { return ch.front(); });
    EXPECT_FALSE(card(kTheBoss)->applyReplacement(ctx, u)) << "declined";
    EXPECT_TRUE(state.getObject(rune).location.has_value());

    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) { return pickObj(ch, u); });
    EXPECT_TRUE(card(kTheBoss)->applyReplacement(ctx, u));
    EXPECT_FALSE(state.getObject(rune).location.has_value()) << "the rune paid [C]";
    EXPECT_EQ(state.getObject(u).buff_count, 0);
    EXPECT_EQ(state.getObject(u).zone, ZoneType::Base);
    EXPECT_TRUE(state.getObject(boss).is_exhausted);
}

TEST_F(OlderSetGaps2, CostChanges_LedrosMasterYiSwiftScout) {
    const auto ledros = card(kCommanderLedros)->optionalAdditionalCost();
    EXPECT_TRUE(ledros.kill_any_number);
    EXPECT_EQ(ledros.power_reduction_per_kill, 1);

    const Card* yi = card(kMasterYi);
    auto& ps = state.player(P1);
    ps.xp = 2;  EXPECT_EQ(yi->selfPowerCostReduction(state, P1), 0);
    ps.xp = 3;  EXPECT_EQ(yi->selfPowerCostReduction(state, P1), 1);
    ps.xp = 6;  EXPECT_EQ(yi->selfPowerCostReduction(state, P1), 2);
    ps.xp = 11; EXPECT_EQ(yi->selfPowerCostReduction(state, P1), 3);
    const auto yi_obj = addUnit(P1, kMasterYi);
    card(kMasterYi)->applyPassiveAura(state, P1);
    EXPECT_FALSE(state.getObject(yi_obj).untargetable_by_enemy);
    ps.xp = 16;
    card(kMasterYi)->applyPassiveAura(state, P1);
    EXPECT_TRUE(state.getObject(yi_obj).untargetable_by_enemy);

    const auto scout = addUnit(P1, kSwiftScout);
    card(kSwiftScout)->applyPassiveAura(state, P1);
    EXPECT_FALSE(ps.hide_for_energy) << "not in the Legend Zone";
    state.getObject(scout).zone = ZoneType::LegendZone;
    card(kSwiftScout)->applyPassiveAura(state, P1);
    EXPECT_TRUE(ps.hide_for_energy);
}

TEST_F(OlderSetGaps2, EnterReady_MagmaWurmAndWujuMaster) {
    const auto wurm = addUnit(P1, kMagmaWurm);
    const auto other = addUnit(P1, kInvalidId, 2);
    EXPECT_TRUE(card(kMagmaWurm)->grantsEnterReadyTo(state, wurm, state.getObject(other)));
    EXPECT_FALSE(card(kMagmaWurm)->grantsEnterReadyTo(state, wurm, state.getObject(wurm)));

    const auto wuju = addUnit(P1, kWujuMaster);
    state.player(P1).xp = 10;
    EXPECT_FALSE(card(kWujuMaster)->grantsEnterReadyTo(state, wuju, state.getObject(other)));
    state.player(P1).xp = 11;
    EXPECT_TRUE(card(kWujuMaster)->grantsEnterReadyTo(state, wuju, state.getObject(other)));
}

TEST_F(OlderSetGaps2, VoidAssault_YoureTheAttacker) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto mine = addUnit(P1, kInvalidId, 2);
    const auto theirs = addUnit(P2, kInvalidId, 2);
    const auto src = pushSpellOnChain(P1, kVoidAssault);
    state.chain.items.clear();
    ChainItem ri;
    ri.id = state.chain.allocateId();
    ri.controller = P1;
    ri.is_spell = true;
    ri.source = src;
    ri.card_def_id = kVoidAssault;
    state.chain.resuming = ri;
    CardContext ctx{state, events, exec, P1, src};
    const std::vector<GameObjectId> targets{mine, theirs};
    card(kVoidAssault)->onResolve(ctx, targets);
    while (exec.hasPendingChoice()) {
        auto pending = exec.consumePendingChoice();
        exec.recordChoice(pickValue(pending.legal, 1));   // battlefield 0
        card(kVoidAssault)->onResolve(ctx, targets);
    }
    state.chain.resuming.reset();
    const auto& bf = state.battlefields[0];
    EXPECT_EQ(state.getObject(mine).battlefieldId(), bf.id);
    EXPECT_EQ(state.getObject(theirs).battlefieldId(), bf.id);
    EXPECT_TRUE(bf.is_contested);
    EXPECT_EQ(bf.contested_by, P1);
}

}  // namespace
}  // namespace riftbound::test
