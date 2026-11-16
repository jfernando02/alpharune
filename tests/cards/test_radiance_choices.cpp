/// @file test_radiance_choices.cpp
/// Radiance cards whose choices and batched triggers used to be approximated:
/// simultaneous standard moves (CR 144.3), "one or more units die" batches,
/// Bilgewater Tracker's "up to 2", The Empyrean's Show Off, Command: Attack's
/// modes, Blossoming Dreamroot, Hunters' Circle and Orianna's discount.

#include "tests/cards/card_test_fixture.h"

#include "agents/agent_interface.h"
#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/chain_manager.h"
#include "engine/effect_executor.h"
#include "engine/game_engine.h"
#include "engine/trigger_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

namespace riftbound::test {
namespace {

constexpr CardDefId kBilgewaterTracker   = 958;
constexpr CardDefId kBlossomingDreamroot = 974;
constexpr CardDefId kCommandAttack       = 983;
constexpr CardDefId kTheEmpyrean         = 1006;
constexpr CardDefId kOrianna             = 1008;
constexpr CardDefId kIlluminators        = 1055;
constexpr CardDefId kHuntersCircle       = 1083;

struct FnAgent : AgentInterface {
    std::function<Intent(const std::vector<Intent>&)> f;
    Intent selectAction(const GameState&, const std::vector<Intent>& legal) override { return f(legal); }
};

// The option naming `id`, else the one with chosen_value `v`.
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

class RadianceChoices : public CardTestFixture {
protected:
    int itemsFrom(CardDefId id) {
        int n = 0;
        for (const auto& it : state.chain.items) if (it.card_def_id == id) ++n;
        return n;
    }
};

// CR 144.3: a standard move may take other units along to the same place.
TEST_F(RadianceChoices, StandardMoveTakesOtherUnitsAlong) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = engine.mutableState();
    s.players[0].id = P1;
    s.players[1].id = P2;
    s.turn.turn_player = P1;
    s.turn.turn_number = 3;
    s.turn.phase = TurnPhase::MainPhase;
    BattlefieldState b0; b0.id = 0; s.battlefields.push_back(b0);
    BattlefieldState b1; b1.id = 1; s.battlefields.push_back(b1);
    auto unit = [&](bool exhausted) {
        auto id = s.createObject();
        auto& u = s.getObject(id);
        u.owner = P1; u.controller = P1; u.card_type = CardType::Unit;
        u.name = "Mover"; u.base_might = 2; u.current_might = 2;
        u.zone = ZoneType::Base; u.location = BaseLocation{P1};
        u.is_exhausted = exhausted;
        return id;
    };
    const auto a = unit(false), b = unit(false), c = unit(false), tired = unit(true);

    bool offered_tired = false;
    int asks = 0;
    FnAgent agent;
    agent.f = [&](const std::vector<Intent>& legal) {
        for (const auto& i : legal)
            if (!i.chosen_objects.empty() && i.chosen_objects[0] == tired) offered_tired = true;
        if (legal.front().type == IntentType::MakeChoice && legal.front().chosen_value == 0 &&
            legal.size() > 1 && asks++ == 0)
            return pickObj(legal, b);   // take b along, then stop
        return legal.front();
    };
    engine.testHook_setAgents(&agent, &agent);

    Intent move;
    move.type = IntentType::StandardMove;
    move.player = P1;
    move.units_to_move = {a};
    move.move_destination = BattlefieldLocation{0};
    engine.testHook_executeIntent(move);

    EXPECT_EQ(s.getObject(a).battlefieldId(), std::optional<BattlefieldId>(0));
    EXPECT_EQ(s.getObject(b).battlefieldId(), std::optional<BattlefieldId>(0)) << "moved along";
    EXPECT_FALSE(s.getObject(c).battlefieldId().has_value()) << "the player stopped after b";
    EXPECT_FALSE(offered_tired) << "an exhausted unit can't make a standard move";
    EXPECT_TRUE(s.getObject(b).is_exhausted);
}

// "When one or more friendly units die": once for units that die together.
TEST_F(RadianceChoices, Illuminators_OncePerDeathBatch) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    addUnit(P1, kIlluminators, 2);
    const auto a = addUnit(P1, kInvalidId, 1), b = addUnit(P1, kInvalidId, 1), c = addUnit(P1, kInvalidId, 1);

    state.death_batch++;
    exec.killObject(a);
    exec.killObject(b);
    EXPECT_EQ(itemsFrom(kIlluminators), 1) << "two units died at the same time";
    state.death_batch++;
    exec.killObject(c);
    EXPECT_EQ(itemsFrom(kIlluminators), 2) << "a later death triggers again";
}

TEST_F(RadianceChoices, BilgewaterTracker_NoneOneOrTwoOfAnyUnits) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto me = addUnit(P1, kBilgewaterTracker, 2);
    const auto mine = addUnit(P1, kInvalidId, 3);
    const auto theirs = addUnit(P2, kInvalidId, 3);

    driveResumableTrigger(kBilgewaterTracker, P1, me,
                          [](const std::vector<Intent>& l) { return pickValue(l, 0); }, exec);
    EXPECT_EQ(state.getObject(theirs).damage_marked, 0) << "dealt to no unit";

    int step = 0;
    driveResumableTrigger(kBilgewaterTracker, P1, me, [&](const std::vector<Intent>& l) {
        return step++ == 0 ? pickValue(l, 1) : pickObj(l, mine);
    }, exec);
    EXPECT_EQ(state.getObject(mine).damage_marked, 1) << "a friendly unit may be chosen";
    EXPECT_EQ(state.getObject(theirs).damage_marked, 0) << "only one unit";
}

TEST_F(RadianceChoices, Empyrean_PredictsThePowerOfTheChosenCard) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    exec.setAgentQuery([](PlayerId, const std::vector<Intent>& ch) { return ch.front(); });
    CardDefId two = kInvalidId;
    for (CardDefId cid : card_registry.classDefIds()) if (const Card* c = card_registry.get(cid))
        if (c->def().power_cost == 2 && c->def().card_type == CardType::Spell) { two = cid; break; }
    ASSERT_NE(two, kInvalidId);
    const auto shown = addToHand(P1, two);
    for (int i = 0; i < 5; ++i) addToDeck(P1, two);
    const auto me = addUnit(P1, kTheEmpyrean, 4);
    const int hand = handSize(P1);
    driveResumableTrigger(kTheEmpyrean, P1, me,
                          [&](const std::vector<Intent>& l) { return pickObj(l, shown); }, exec);
    EXPECT_EQ(handSize(P1), hand + 2) << "showed off a card: draw 2";
}

TEST_F(RadianceChoices, CommandAttack_MovesADeployGear) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const auto gear = addUnit(P1, kBlossomingDreamroot, 0, 0);
    state.getObject(gear).card_type = CardType::Gear;
    state.getObject(gear).deploy = true;
    const auto src = pushSpellOnChain(P1, kCommandAttack);
    state.chain.items.clear();
    int step = 0;
    driveResumable(kCommandAttack, P1, src, [&](const std::vector<Intent>& l) {
        switch (step++) {
            case 0: return pickValue(l, 1);     // mode: move a [Deploy] gear
            case 1: return pickValue(l, 1);     // to battlefield #1
            default: return pickObj(l, gear);

        }
    }, exec);
    EXPECT_EQ(state.getObject(gear).battlefieldId(), std::optional<BattlefieldId>(1));
}

TEST_F(RadianceChoices, BlossomingDreamroot_DrawsWhenYouWinACombatHere) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.subscribe();
    const auto gear = addUnit(P1, kBlossomingDreamroot, 0, 0);
    state.getObject(gear).card_type = CardType::Gear;

    events.emit(CombatEndedEvent{1, P1});
    EXPECT_EQ(itemsFrom(kBlossomingDreamroot), 0) << "a combat elsewhere";
    events.emit(CombatEndedEvent{0, P2});
    EXPECT_EQ(itemsFrom(kBlossomingDreamroot), 0) << "the opponent won";
    events.emit(CombatEndedEvent{0, P1});
    EXPECT_EQ(itemsFrom(kBlossomingDreamroot), 1);
}

TEST_F(RadianceChoices, HuntersCircle_FirstPermanentHereEachTurn) {
    const Card* c = card_registry.get(kHuntersCircle);
    ASSERT_NE(c, nullptr);
    state.turn.turn_player = P1;
    const auto u = addToHand(P1, kBilgewaterTracker);
    EXPECT_EQ(c->playHereDiscount(state, 0, P1, state.getObject(u)), 1);
    EXPECT_EQ(c->playHereDiscount(state, 0, P2, state.getObject(u)), 0) << "not on P2's turn";
    state.player(P1).bfs_played_permanent_this_turn.push_back(0);
    EXPECT_EQ(c->playHereDiscount(state, 0, P1, state.getObject(u)), 0);
    state.player(P1).resetTurnTracking();
    EXPECT_EQ(c->playHereDiscount(state, 0, P1, state.getObject(u)), 1);
}

TEST_F(RadianceChoices, Orianna_DiscountIsOncePerTurnAndOptional) {
    const Card* o = card_registry.get(kOrianna);
    const auto me = addUnit(P1, kOrianna, 4, 0);
    o->applyPassiveAura(state, P1);
    const auto& mods = state.player(P1).cost_modifiers;
    ASSERT_EQ(mods.size(), 1u);
    EXPECT_TRUE(mods[0].once_per_turn_optional);
    EXPECT_FALSE(mods[0].first_gear_per_turn) << "not tied to the first gear played";
    EXPECT_EQ(mods[0].source, me);
}

}  // namespace
}  // namespace riftbound::test
