/// @file test_older_set_gaps.cpp
/// Cards from the sets before Radiance whose effects used to be approximated
/// or left out: delayed abilities that outlive their source (Ashe, Blue
/// Sentinel, Noxian Guillotine), choices the engine used to make (Disposal
/// Order, Guerilla Warfare, Vision, Unlicensed Armory, Sivir), turn trackers
/// (Spoils of War, Vex), kill attribution (Solari Shrine) and conditions
/// (Yeti Brawler, Harpoon Squad, Gentle Gemdragon, Minotaur Reckoner).

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

constexpr CardDefId kUnlicensedArmory = 23;
constexpr CardDefId kSolariShrine     = 72;
constexpr CardDefId kGemcraftSeer     = 100;
constexpr CardDefId kSpoilsOfWar      = 144;
constexpr CardDefId kNoxianGuillotine = 251;
constexpr CardDefId kGuerillaWarfare  = 260;
constexpr CardDefId kBushwhack        = 327;
constexpr CardDefId kMinotaurReckoner = 337;
constexpr CardDefId kSivir            = 441;
constexpr CardDefId kHarpoonSquad     = 458;
constexpr CardDefId kYetiBrawler      = 580;
constexpr CardDefId kBlueSentinel     = 649;
constexpr CardDefId kDisposalOrder    = 665;
constexpr CardDefId kGentleGemdragon  = 666;
constexpr CardDefId kVexApathetic     = 712;
constexpr CardDefId kAsheFocused      = 731;
constexpr CardDefId kHeimerdinger     = 111;
constexpr CardDefId kQuartermaster    = 367;
constexpr CardDefId kYone             = 544;
constexpr CardDefId kStalkingWolf     = 728;
constexpr CardDefId kPoppy            = 740;

Intent pickObj(const std::vector<Intent>& legal, GameObjectId id) {
    for (const auto& i : legal)
        if (std::find(i.chosen_objects.begin(), i.chosen_objects.end(), id) != i.chosen_objects.end() ||
            std::find(i.targets.begin(), i.targets.end(), id) != i.targets.end())
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

class OlderSetGaps : public CardTestFixture {
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
    GameObjectId addGear(PlayerId owner, CardDefId def_id) {
        const auto id = addUnit(owner, def_id);
        state.getObject(id).card_type = CardType::Gear;
        return id;
    }
    Card* card(CardDefId id) { return card_registry.get(id); }
};

TEST_F(OlderSetGaps, SpoilsOfWar_CostsLessOnlyAfterAnEnemyUnitDiedThisTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto mine = addUnit(P1, kInvalidId, 1);
    exec.killObject(mine);
    EXPECT_EQ(card(kSpoilsOfWar)->selfCostReduction(state, P1), 0) << "a friendly unit died";
    exec.killObject(addUnit(P2, kInvalidId, 1));
    EXPECT_EQ(card(kSpoilsOfWar)->selfCostReduction(state, P1), 2);
    state.player(P2).resetTurnTracking();
    EXPECT_EQ(card(kSpoilsOfWar)->selfCostReduction(state, P1), 0) << "not a unit in the trash";
}

TEST_F(OlderSetGaps, VexApathetic_TheUnitCantMoveThisTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    state.turn.turn_number = 5;
    const auto vex = addUnit(P1, kVexApathetic, 0, 0);
    const auto played = addUnit(P2, kInvalidId, 3, 1);
    state.getObject(vex).card_counters["__opp_played_unit_id"] = static_cast<int>(played);
    driveResumableTrigger(kVexApathetic, P1, vex,
                          [](const std::vector<Intent>& l) { return l.front(); }, exec);
    EXPECT_TRUE(state.getObject(played).is_stunned);
    EXPECT_EQ(state.getObject(played).cant_move_on_turn, 5);
}

TEST_F(OlderSetGaps, NoxianGuillotine_KillsTheNextTimeItTakesDamage) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    const auto unit = addUnit(P2, kInvalidId, 5, 0);
    const auto other = addUnit(P2, kInvalidId, 5, 0);
    const auto src = pushSpellOnChain(P1, kNoxianGuillotine);
    state.chain.items.clear();
    state.player(P1).cards_played_this_turn = 1;   // no [Legion]
    CardContext ctx{state, events, exec, P1, src};
    card(kNoxianGuillotine)->onResolve(ctx, {unit});
    EXPECT_TRUE(state.getObject(unit).location.has_value()) << "not killed now";

    exec.dealDamage(other, 1, kInvalidId);
    EXPECT_EQ(itemsFrom(kNoxianGuillotine), 0) << "another unit took damage";
    exec.dealDamage(unit, 1, kInvalidId);
    ASSERT_EQ(itemsFrom(kNoxianGuillotine), 1);
    EXPECT_EQ(state.chain.items.back().triggering_subject, unit);
    exec.dealDamage(unit, 1, kInvalidId);
    EXPECT_EQ(itemsFrom(kNoxianGuillotine), 1) << "only the next time";

    driveResumableTrigger(kNoxianGuillotine, P1, src,
                          [](const std::vector<Intent>& l) { return l.front(); }, exec,
                          TriggerType::WhenItTakesDamage, unit);
    EXPECT_FALSE(state.getObject(unit).location.has_value()) << "killed";
}

TEST_F(OlderSetGaps, GuerillaWarfare_ChoosesUpToTwoAndHidesForFree) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    CardDefId hidden = kInvalidId;
    for (CardDefId cid : card_registry.classDefIds())
        if (card_db.get(cid).keywords.has(Keyword::Hidden) && cid != kGuerillaWarfare) { hidden = cid; break; }
    ASSERT_NE(hidden, kInvalidId);
    const auto a = addToTrash(P1, hidden), b = addToTrash(P1, hidden), c = addToTrash(P1, hidden);
    int asks = 0;
    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) {
        // decline first; take c, then decline.
        return asks++ == 0 ? pickObj(ch, c) : ch.front();
    });
    const auto src = pushSpellOnChain(P1, kGuerillaWarfare);
    state.chain.items.clear();
    CardContext ctx{state, events, exec, P1, src};
    card(kGuerillaWarfare)->onResolve(ctx, {});
    EXPECT_TRUE(inHand(P1, c));
    EXPECT_TRUE(inTrash(P1, a));
    EXPECT_TRUE(inTrash(P1, b)) << "the player stopped after one";
    EXPECT_TRUE(state.player(P1).hide_ignoring_costs_this_turn);
}

TEST_F(OlderSetGaps, DisposalOrder_ThePlayerChoosesTheCards) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    std::vector<GameObjectId> trash;
    for (int i = 0; i < 4; ++i) trash.push_back(addToTrash(P2, kSpoilsOfWar));
    int asks = 0;
    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) {
        switch (asks++) {
            case 0: return pickObj(ch, trash[0]);
            case 1: return pickObj(ch, trash[3]);
            default: return ch.front();   // stop at two
        }
    });
    const auto src = pushSpellOnChain(P1, kDisposalOrder);
    state.chain.items.clear();
    driveResumable(kDisposalOrder, P1, src,
                   [](const std::vector<Intent>& l) { return pickValue(l, 0); }, exec);
    EXPECT_EQ(trashSize(P2), 2);
    EXPECT_TRUE(inTrash(P2, trash[1]));
    EXPECT_TRUE(inTrash(P2, trash[2]));
}

TEST_F(OlderSetGaps, AsheFocused_TheCardReturnsWhenTheyHold) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    const auto ashe = addUnit(P1, kAsheFocused);
    const auto c = addToHand(P2, kSpoilsOfWar);
    driveResumableTrigger(kAsheFocused, P1, ashe,
                          [&](const std::vector<Intent>& l) { return pickObj(l, c); }, exec,
                          TriggerType::WhenYouPlayMe);
    EXPECT_EQ(state.getObject(c).zone, ZoneType::Banishment);

    exec.killObject(ashe);   // "even if I'm no longer on the board"
    events.emit(ScoreEvent{P1, 0, ScoreMethod::Hold, 1});
    EXPECT_EQ(itemsFrom(kAsheFocused), 0) << "I held, not them";
    events.emit(ScoreEvent{P2, 0, ScoreMethod::Hold, 1});
    ASSERT_EQ(itemsFrom(kAsheFocused), 1);
    driveResumableTrigger(kAsheFocused, P1, ashe,
                          [](const std::vector<Intent>& l) { return l.front(); }, exec,
                          TriggerType::WhenAnOpponentHolds, state.chain.items.back().triggering_subject);
    EXPECT_TRUE(inHand(P2, c));
}

TEST_F(OlderSetGaps, BlueSentinel_HoldsTwiceAndAddsAtTheNextMainPhase) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    const auto me = addUnit(P1, kBlueSentinel, 0, 0);
    events.emit(ScoreEvent{P1, 0, ScoreMethod::Hold, 1});
    EXPECT_EQ(itemsFrom(kBlueSentinel), 2) << "my hold effect here triggers an additional time";
    state.chain.items.clear();

    const int before = state.player(P1).rune_pool.universal_power;
    driveResumableTrigger(kBlueSentinel, P1, me,
                          [](const std::vector<Intent>& l) { return l.front(); }, exec,
                          TriggerType::WhenIHold);
    EXPECT_EQ(state.player(P1).rune_pool.universal_power, before) << "not now";
    events.emit(PhaseChangedEvent{TurnPhase::BeginningStep, TurnPhase::MainPhase, P2});
    EXPECT_EQ(itemsFrom(kBlueSentinel), 0) << "the opponent's Main Phase";
    events.emit(PhaseChangedEvent{TurnPhase::BeginningStep, TurnPhase::MainPhase, P1});
    ASSERT_EQ(itemsFrom(kBlueSentinel), 1);
    driveResumableTrigger(kBlueSentinel, P1, me,
                          [](const std::vector<Intent>& l) { return l.front(); }, exec,
                          TriggerType::AtStartOfMain);
    EXPECT_EQ(state.player(P1).rune_pool.universal_power, before + 1);
}

TEST_F(OlderSetGaps, SolariShrine_WhenYouKillAStunnedEnemyUnit) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    addGear(P1, kSolariShrine);
    ChainItem mine; mine.controller = P1; mine.is_spell = true;
    state.chain.resuming = mine;   // P1's spell is killing

    exec.killObject(addUnit(P2, kInvalidId, 2));
    EXPECT_EQ(itemsFrom(kSolariShrine), 0) << "not stunned";
    const auto stunned = addUnit(P2, kInvalidId, 2);
    state.getObject(stunned).is_stunned = true;
    exec.killObject(stunned);
    EXPECT_EQ(itemsFrom(kSolariShrine), 1);

    ChainItem theirs; theirs.controller = P2; theirs.is_spell = true;
    state.chain.resuming = theirs;
    const auto s2 = addUnit(P2, kInvalidId, 2);
    state.getObject(s2).is_stunned = true;
    exec.killObject(s2);
    EXPECT_EQ(itemsFrom(kSolariShrine), 1) << "the opponent killed it";
}

TEST_F(OlderSetGaps, UnlicensedArmory_RecallOnlyIfThePlayerPays) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto arm = [&](GameObjectId u) {
        state.getObject(u).death_replacement_recall_pending = true;
        state.getObject(u).death_replacement_recall_by = kUnlicensedArmory;
    };
    const auto rune = addRune(P1, Domain::Fury);
    const auto a = addUnit(P1, kInvalidId, 2, 0);
    arm(a);
    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) { return pickObj(ch, a); });
    exec.killObject(a);
    EXPECT_EQ(state.getObject(a).zone, ZoneType::Base) << "paid [C]: recalled";
    EXPECT_FALSE(state.getObject(rune).location.has_value()) << "the rune paid for it";

    addRune(P1, Domain::Fury);
    const auto b = addUnit(P1, kInvalidId, 2, 0);
    arm(b);
    exec.setAgentQuery([](PlayerId, const std::vector<Intent>& ch) { return ch.front(); });
    exec.killObject(b);
    EXPECT_TRUE(inTrash(P1, b)) << "declined: it dies";
}

TEST_F(OlderSetGaps, ExcessDamageConditions_YetiAndSivir) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto yeti = addUnit(P1, kYetiBrawler, 0, 0);
    auto& ps = state.player(P1);
    ps.last_combat_bf = 0;
    ps.last_combat_attacker = true;
    ps.excess_combat_damage = 2;
    EXPECT_FALSE(card(kYetiBrawler)->triggerCondition(state, yeti, P1, TriggerType::WhenIConquer, kInvalidId));
    ps.excess_combat_damage = 3;
    EXPECT_TRUE(card(kYetiBrawler)->triggerCondition(state, yeti, P1, TriggerType::WhenIConquer, kInvalidId));

    const auto sivir = addUnit(P1, kSivir, 0, 0);
    EXPECT_FALSE(card(kSivir)->triggerCondition(state, sivir, P1, TriggerType::WhenIConquer, kInvalidId));
    ps.excess_combat_damage = 6;
    ASSERT_TRUE(card(kSivir)->triggerCondition(state, sivir, P1, TriggerType::WhenIConquer, kInvalidId));
    const auto enemy = addUnit(P2, kInvalidId, 9);
    int step = 0;
    driveResumableTrigger(kSivir, P1, sivir, [&](const std::vector<Intent>& l) {
        return step++ == 0 ? l.back() : pickObj(l, enemy);   // yes, then the unit
    }, exec, TriggerType::WhenIConquer);
    EXPECT_EQ(state.getObject(enemy).damage_marked, 6) << "deal that much";
}

TEST_F(OlderSetGaps, HarpoonSquad_OnlyMovingFromABattlefield) {
    const auto me = addUnit(P1, kHarpoonSquad, 0, 0);
    auto& c = state.getObject(me).card_counters;
    c["__moved_from_bf"] = -1;
    EXPECT_FALSE(card(kHarpoonSquad)->triggerCondition(state, me, P1, TriggerType::WhenIMove, kInvalidId));
    c["__moved_from_bf"] = 1;
    EXPECT_TRUE(card(kHarpoonSquad)->triggerCondition(state, me, P1, TriggerType::WhenIMove, kInvalidId));
}

TEST_F(OlderSetGaps, GentleGemdragon_AnotherDragon) {
    const auto me = addUnit(P1, kGentleGemdragon);
    const auto plain = addUnit(P1, kInvalidId, 2);
    const auto dragon = addUnit(P1, kInvalidId, 2);
    state.getObject(dragon).tags = {"Dragon"};
    const Card* c = card(kGentleGemdragon);
    EXPECT_FALSE(c->triggerCondition(state, me, P1, TriggerType::WhenYouPlayAUnit, plain));
    EXPECT_TRUE(c->triggerCondition(state, me, P1, TriggerType::WhenYouPlayAUnit, dragon));
    EXPECT_TRUE(c->triggerCondition(state, me, P1, TriggerType::WhenYouPlayMe, me));
}

TEST_F(OlderSetGaps, MinotaurReckoner_RestrictionEndsWhenItLeaves) {
    const auto me = addUnit(P2, kMinotaurReckoner, 0, 0);
    card(kMinotaurReckoner)->applyPassiveAura(state, P2);
    for (const auto& bf : state.battlefields) EXPECT_TRUE(bf.aura_blocks_move_to_base);
    state.getObject(me).location.reset();
    for (auto& bf : state.battlefields) bf.aura_blocks_move_to_base = false;   // the aura pass
    card(kMinotaurReckoner)->applyPassiveAura(state, P2);
    for (const auto& bf : state.battlefields) EXPECT_FALSE(bf.aura_blocks_move_to_base);
}

TEST_F(OlderSetGaps, Vision_ThePlayerChoosesAndGrantsCount) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.setEffectExecutor(&exec);
    tm.subscribe();
    addUnit(P1, kGemcraftSeer);
    const auto bottom = addToDeck(P1, kSpoilsOfWar);
    const auto top = addToDeck(P1, kBushwhack);
    ASSERT_EQ(state.player(P1).main_deck.back(), top);
    const auto played = addUnit(P1, kHarpoonSquad);   // no printed [Vision]

    exec.setAgentQuery([](PlayerId, const std::vector<Intent>& ch) { return ch.front(); });
    events.emit(CardPlayedEvent{played, P1, CardType::Unit, 1, 2});
    EXPECT_EQ(state.player(P1).main_deck.back(), top) << "declined: stays on top";

    exec.setAgentQuery([&](PlayerId, const std::vector<Intent>& ch) { return pickObj(ch, top); });
    events.emit(CardPlayedEvent{played, P1, CardType::Unit, 2, 2});
    EXPECT_EQ(state.player(P1).main_deck.front(), top) << "recycled to the bottom";
    EXPECT_EQ(state.player(P1).main_deck.back(), bottom);
}

TEST_F(OlderSetGaps, Bushwhack_FriendlyUnitsEnterReadyThisTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto src = pushSpellOnChain(P1, kBushwhack);
    state.chain.items.clear();
    CardContext ctx{state, events, exec, P1, src};
    card(kBushwhack)->onResolve(ctx, {});
    EXPECT_TRUE(state.player(P1).units_enter_ready_this_turn);
}

TEST_F(OlderSetGaps, Yone_OnlyAfterConqueringAnUncontrolledBattlefield) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto yone = addUnit(P1, kYone, 4, 0);
    state.battlefields[0].conquered_from_uncontrolled = false;
    EXPECT_FALSE(card(kYone)->triggerCondition(state, yone, P1, TriggerType::WhenIConquer, kInvalidId));
    state.battlefields[0].conquered_from_uncontrolled = true;
    ASSERT_TRUE(card(kYone)->triggerCondition(state, yone, P1, TriggerType::WhenIConquer, kInvalidId));
    addUnit(P2, kInvalidId, 9);
    const auto chosen = addUnit(P2, kInvalidId, 9);
    driveResumableTrigger(kYone, P1, yone,
                          [&](const std::vector<Intent>& l) { return pickObj(l, chosen); }, exec,
                          TriggerType::WhenIConquer);
    EXPECT_EQ(state.getObject(chosen).damage_marked, 4) << "the chosen unit, my Might";
}

TEST_F(OlderSetGaps, Heimerdinger_GrantsEveryExhaustAbility) {
    const auto heimer = addUnit(P1, kHeimerdinger);
    addGear(P1, kSolariShrine);
    card(kHeimerdinger)->applyPassiveAura(state, P1);
    const auto& refs = state.getObject(heimer).granted_abilities;
    EXPECT_GT(refs.size(), 1u) << "not just ability 0";
    for (const auto& r : refs) EXPECT_TRUE(r.exhaust_cost_only);
}

TEST_F(OlderSetGaps, AdditionalCosts_PoppyQuartermasterStalkingWolf) {
    const auto poppy = card(kPoppy)->optionalAdditionalCost();
    EXPECT_EQ(poppy.spend_xp, 3);
    EXPECT_EQ(poppy.reduce_energy, 3);
    const auto qm = card(kQuartermaster)->optionalAdditionalCost();
    EXPECT_TRUE(qm.return_friendly_gear && qm.required);

    const Card* wolf = card(kStalkingWolf);
    const auto wc = wolf->optionalAdditionalCost();
    EXPECT_TRUE(wc.kill_friendly_unit && wc.required);
    const auto plain = addUnit(P1, kInvalidId, 2);
    EXPECT_FALSE(wolf->additionalCostAccepts(state, state.getObject(plain)));
    EXPECT_FALSE(wolf->canBePlayed(state, P1)) << "no Bird, Cat, Dog or Poro";
    state.getObject(plain).tags = {"Cat"};
    EXPECT_TRUE(wolf->additionalCostAccepts(state, state.getObject(plain)));
    EXPECT_TRUE(wolf->canBePlayed(state, P1));
}

}  // namespace
}  // namespace riftbound::test
