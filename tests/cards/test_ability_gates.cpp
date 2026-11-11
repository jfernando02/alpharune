// Activated-ability gates in a Showdown. The Showdown generator used to offer an
// [Action] ability after checking only its exhaust cost and declared targets, so a
// "use only once per turn" ability (Azir, Ascendant) could be activated again and
// again, and power / energy costs were never checked. The main-phase, Showdown
// and Reaction generators now share GameEngine::abilityActivatable.

#include "tests/cards/card_test_fixture.h"

#include "engine/effect_executor.h"
#include "engine/game_engine.h"

namespace riftbound::test {
namespace {

constexpr CardDefId kAzirAscendant = 373;
constexpr CardDefId kDiviningShells = 723;
constexpr CardDefId kShepherdsHeirloom = 720;
constexpr CardDefId kCrowdFavorite = 664;
constexpr CardDefId kMegatusk = 688;

class AbilityGatesTest : public CardTestFixture {
protected:
    // A Showdown at battlefield 0 in P1's main phase, P1 holding focus.
    GameState& showdownState(GameEngine& engine) {
        auto& s = engine.mutableState();
        s.mode = ModeOfPlay{};
        s.players[0].id = P1;
        s.players[1].id = P2;
        s.turn.turn_player = P1;
        s.turn.turn_number = 3;
        s.turn.phase = TurnPhase::MainPhase;
        s.turn.ns_state = NeutralShowdownState::Showdown;
        s.turn.oc_state = OpenClosedState::Open;
        s.turn.focus_holder = P1;
        BattlefieldState b0; b0.id = 0; s.battlefields.push_back(b0);
        BattlefieldState b1; b1.id = 1; s.battlefields.push_back(b1);
        return s;
    }

    GameObjectId place(GameState& s, CardDefId def_id, LocationId where) {
        auto id = s.createObject();
        auto& o = s.getObject(id);
        const auto& def = card_db.get(def_id);
        o.owner = P1; o.controller = P1;
        o.card_def_id = def_id;
        o.name = def.name;
        o.card_type = def.card_type;
        o.base_might = def.might;
        o.current_might = o.base_might;
        o.zone = ZoneType::Base;
        o.location = where;
        o.is_exhausted = false;
        return id;
    }

    GameObjectId plainUnit(GameState& s, LocationId where) {
        auto id = s.createObject();
        auto& u = s.getObject(id);
        u.owner = P1; u.controller = P1;
        u.card_type = CardType::Unit;
        u.base_might = 2; u.current_might = 2;
        u.zone = ZoneType::Base;
        u.location = where;
        return id;
    }

    static bool offered(const std::vector<Intent>& actions, GameObjectId source) {
        for (const auto& a : actions)
            if (a.type == IntentType::ActivateActionAbility && a.ability_source == source) return true;
        return false;
    }
};

// "Use only once per turn": after one use this turn, a Showdown must not offer it again.
TEST_F(AbilityGatesTest, AzirOncePerTurnHoldsInShowdown) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    auto azir = place(s, kAzirAscendant, BattlefieldLocation{0});
    plainUnit(s, BaseLocation{P1});
    s.player(P1).rune_pool.power[static_cast<int>(Domain::Calm)] = 2;  // pays [C] twice

    ASSERT_TRUE(offered(engine.generateLegalActions(), azir))
        << "Azir's [Action] ability should be offered in a Showdown before its first use";

    s.getObject(azir).card_counters["azir_used_turn"] = s.turn.turn_number + 1;
    EXPECT_FALSE(offered(engine.generateLegalActions(), azir))
        << "Azir's ability was already used this turn";

    // Next turn it is available again.
    s.turn.turn_number += 1;
    EXPECT_TRUE(offered(engine.generateLegalActions(), azir));
}

// Power costs are checked in a Showdown too.
TEST_F(AbilityGatesTest, UnaffordableActionAbilityNotOfferedInShowdown) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    auto azir = place(s, kAzirAscendant, BattlefieldLocation{0});
    plainUnit(s, BaseLocation{P1});  // no runes, no power in the pool
    EXPECT_FALSE(offered(engine.generateLegalActions(), azir))
        << "Azir's [C] cost cannot be paid";
}

// An exhaust-cost [Action] ability needs a ready source; a ready effect makes it usable again.
TEST_F(AbilityGatesTest, ExhaustCostAbilityNeedsReadySourceAndReadyEffectRestoresIt) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    auto shells = place(s, kDiviningShells, BaseLocation{P1});
    plainUnit(s, BattlefieldLocation{0});

    ASSERT_TRUE(offered(engine.generateLegalActions(), shells));
    s.getObject(shells).is_exhausted = true;  // as after paying [E]
    EXPECT_FALSE(offered(engine.generateLegalActions(), shells));

    EffectExecutor exec(s, events, card_db, &card_registry);
    exec.readyObject(shells);
    EXPECT_TRUE(offered(engine.generateLegalActions(), shells))
        << "a ready effect allows a second use";
}

// "[Equip] — Spend 1 XP": with no XP the equip did nothing, yet was offered again and again.
TEST_F(AbilityGatesTest, EquipNotOfferedWhenItsCostCannotBePaid) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    s.turn.ns_state = NeutralShowdownState::Neutral;  // main phase, open
    s.turn.focus_holder.reset();
    auto heirloom = place(s, kShepherdsHeirloom, BaseLocation{P1});
    plainUnit(s, BaseLocation{P1});
    const auto equipOffered = [&] {
        for (const auto& a : engine.generateLegalActions())
            if (a.type == IntentType::ActivateAbility && a.ability_source == heirloom) return true;
        return false;
    };
    s.player(P1).xp = 0;
    EXPECT_FALSE(equipOffered());
    s.player(P1).xp = 1;
    EXPECT_TRUE(equipOffered());
}

// XP costs are declared, so the engine offers the ability only when it can pay them and
// pays them on activation. Megatusk used to be offered at any XP (and did nothing below
// 3); a buffed Crowd Favorite was a free no-op that could be repeated forever.
TEST_F(AbilityGatesTest, XpCostAbilitiesNeedTheXpAndPayIt) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    s.turn.ns_state = NeutralShowdownState::Neutral;  // main phase, open
    s.turn.focus_holder.reset();
    auto tusk = place(s, kMegatusk, BattlefieldLocation{0});
    auto fav = place(s, kCrowdFavorite, BattlefieldLocation{0});
    const auto abilityOffered = [&](GameObjectId src) {
        for (const auto& a : engine.generateLegalActions())
            if (a.type == IntentType::ActivateAbility && a.ability_source == src) return true;
        return false;
    };
    s.player(P1).xp = 1;
    EXPECT_FALSE(abilityOffered(tusk));
    EXPECT_FALSE(abilityOffered(fav));
    s.player(P1).xp = 2;
    EXPECT_FALSE(abilityOffered(tusk));
    EXPECT_TRUE(abilityOffered(fav));
    s.player(P1).xp = 3;
    EXPECT_TRUE(abilityOffered(tusk));

    // A buffed Crowd Favorite's ability still costs 2 XP, so it cannot repeat for free.
    s.getObject(fav).buff_count = 1;
    s.player(P1).xp = 1;
    EXPECT_FALSE(abilityOffered(fav));
}

}  // namespace
}  // namespace riftbound::test
