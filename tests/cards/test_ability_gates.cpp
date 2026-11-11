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
constexpr CardDefId kGrandmasterAtArms = 554;
constexpr CardDefId kDoransBlade = 417;
constexpr CardDefId kSettBrawler = 164;
constexpr CardDefId kHextechGauntlets = 748;

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
        o.tags = def.tags;
        o.domains = def.domains;
        o.keywords = def.keywords;
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

// Grandmaster at Arms: "[1], [E]: Attach a detached Equipment you control to a unit you control."
// Equipment you control is Equipment on the board; the ability used to take one from the deck.
TEST_F(AbilityGatesTest, GrandmasterAttachesOnlyEquipmentOnTheBoard) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    s.turn.ns_state = NeutralShowdownState::Neutral;  // main phase, open
    s.turn.focus_holder.reset();
    auto legend = place(s, kGrandmasterAtArms, BaseLocation{P1});
    s.getObject(legend).zone = ZoneType::LegendZone;
    s.getObject(legend).location.reset();
    s.player(P1).legend_zone = legend;
    plainUnit(s, BaseLocation{P1});
    auto rune = s.createObject();
    auto& r = s.getObject(rune);
    r.owner = P1; r.controller = P1;
    r.card_type = CardType::Rune;
    r.domains = {Domain::Calm};
    r.zone = ZoneType::Base;
    r.location = BaseLocation{P1};
    // The Equipment is still in the deck.
    auto blade = place(s, kDoransBlade, BaseLocation{P1});
    s.getObject(blade).zone = ZoneType::MainDeck;
    s.getObject(blade).location.reset();
    s.player(P1).main_deck.push_back(blade);
    const auto attachOffered = [&] {
        for (const auto& a : engine.generateLegalActions())
            if (a.type == IntentType::ActivateAbility && a.ability_source == legend && a.ability_index == 0)
                return true;
        return false;
    };
    EXPECT_FALSE(attachOffered()) << "no Equipment on the board";

    s.player(P1).main_deck.clear();
    s.getObject(blade).zone = ZoneType::Base;
    s.getObject(blade).location = BaseLocation{P1};
    EXPECT_TRUE(attachOffered()) << "a detached Equipment at base";
}

}  // namespace
// Sett, Brawler: "Spend my buff: Give me +4 [M] this turn." The buff is the cost, paid by THIS
// Sett as the ability is activated. The gate used to ask whether any Sett its controller had
// was buffed, and the buff was only taken at resolution, so with one buffed Sett an unbuffed
// copy could activate without end (1,994 activations in one fuzzed game, each doing nothing).
TEST_F(AbilityGatesTest, SpendMyBuffIsPaidByThisUnitAtActivation) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    s.turn.ns_state = NeutralShowdownState::Neutral;
    s.turn.focus_holder.reset();
    auto buffed = place(s, kSettBrawler, BattlefieldLocation{0});
    auto plain = place(s, kSettBrawler, BattlefieldLocation{0});
    s.getObject(buffed).buff_count = 1;
    const auto ability = [&](GameObjectId src) -> std::optional<Intent> {
        for (const auto& a : engine.generateLegalActions())
            if (a.type == IntentType::ActivateAbility && a.ability_source == src) return a;
        return std::nullopt;
    };
    EXPECT_FALSE(ability(plain).has_value()) << "this Sett has no buff to spend";
    EXPECT_TRUE(ability(buffed).has_value());
    // The buff is a declared cost, so the engine takes it as the ability is activated.
    EXPECT_TRUE(card_registry.get(kSettBrawler)->getActivationCost().spend_buff_self);
    s.getObject(buffed).buff_count = 0;   // spent
    EXPECT_FALSE(ability(buffed).has_value());
}

// Hextech Gauntlets: "[Equip] [3][A]. This ability's Energy cost is reduced by the Might of the
// unit you choose." The cost depends on the unit, so only units it can be paid for are offered;
// the generator used to offer every unit once ANY unit made it affordable, and the failed equip
// onto a weak unit repeated (495 times in one fuzzed game).
TEST_F(AbilityGatesTest, EquipWithAUnitDependentCostOffersOnlyPayableUnits) {
    GameEngine engine(card_db, events, card_registry);
    auto& s = showdownState(engine);
    s.turn.ns_state = NeutralShowdownState::Neutral;
    s.turn.focus_holder.reset();
    auto gauntlets = place(s, kHextechGauntlets, BaseLocation{P1});
    auto strong = plainUnit(s, BattlefieldLocation{0});
    auto weak = plainUnit(s, BattlefieldLocation{0});
    s.getObject(strong).current_might = 3;   // [3] - 3 = nothing but the [A]
    s.getObject(weak).current_might = 1;     // still owes [2]
    auto rune = s.createObject();
    auto& r = s.getObject(rune);
    r.owner = P1; r.controller = P1; r.card_type = CardType::Rune;
    r.domains = {Domain::Fury}; r.zone = ZoneType::Base; r.location = BaseLocation{P1};
    std::vector<GameObjectId> targets;
    for (const auto& a : engine.generateLegalActions())
        if (a.ability_source == gauntlets && !a.targets.empty()) targets.push_back(a.targets[0]);
    EXPECT_NE(std::find(targets.begin(), targets.end(), strong), targets.end());
    EXPECT_EQ(std::find(targets.begin(), targets.end(), weak), targets.end())
        << "one rune can't pay [2] and the [A]";
}

}  // namespace riftbound::test
