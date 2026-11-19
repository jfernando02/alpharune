/// @file test_radiance_meta.cpp
/// The Radiance cards the Riven, Kai'Sa and Kennen tournament lists need
/// (decks/meta/), and the engine hooks they brought: "When I become
/// exhausted", "Units you play ... enter ready" and "[Equip] — [A] or exhaust
/// a friendly unit".

#include "tests/cards/card_test_fixture.h"

#include "cards/card.h"
#include "cards/card_helpers.h"
#include "cards/card_registry.h"
#include "core/events.h"
#include "core/game_state.h"
#include "engine/effect_executor.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace riftbound::test {
namespace {

constexpr CardDefId kBilgewaterTracker  = 958;
constexpr CardDefId kRivenBrokenBlade   = 992;
constexpr CardDefId kTheEmpyrean        = 1006;
constexpr CardDefId kAnOffer            = 1041;
constexpr CardDefId kRecklessInnovator  = 1043;
constexpr CardDefId kBedevilingStranger = 1045;
constexpr CardDefId kTrifarianCaptain   = 1064;
constexpr CardDefId kBladeOfTheExile    = 1082;
constexpr CardDefId kRakelstake         = 1097;
constexpr CardDefId kRuinedMonastery    = 1098;
constexpr CardDefId kBattlePreparations = 1100;
constexpr CardDefId kTheExile           = 1087;
constexpr CardDefId kBrutalizer         = 365;   // a 2-cost gear
constexpr CardDefId kSingularity        = 105;   // a 6-cost spell

/// Picks, in order, the option naming each id (chosen_objects) or carrying
/// each value (chosen_value); the first option when there is no match.
std::function<Intent(const std::vector<Intent>&)> pickSeq(std::vector<int64_t> wants) {
    auto order = std::make_shared<std::vector<int64_t>>(std::move(wants));
    auto next = std::make_shared<size_t>(0);
    return [order, next](const std::vector<Intent>& legal) {
        if (*next < order->size()) {
            const int64_t want = (*order)[(*next)++];
            for (const auto& i : legal) {
                if (!i.chosen_objects.empty() && i.chosen_objects.front() == want) return i;
                if (i.chosen_objects.empty() && i.chosen_value && *i.chosen_value == want) return i;
            }
        }
        return legal.empty() ? Intent{} : legal.front();
    };
}

class RadianceMeta : public CardTestFixture {
protected:
    GameObjectId addGear(PlayerId owner, CardDefId def_id) {
        auto id = addUnit(owner, def_id);
        auto& obj = state.getObject(id);
        obj.card_type = CardType::Gear;
        obj.base_might = obj.current_might = 0;
        return id;
    }
    int goldTokens(PlayerId p) {
        int n = 0;
        for (const auto& [id, o] : state.objects)
            if (o.isGear() && o.controller == p && o.card_def_id == kGoldGearCardDefId &&
                o.location.has_value()) {
                ++n;
                EXPECT_TRUE(o.is_exhausted);
            }
        return n;
    }
    std::string root() {
        auto reg = findRegistryPath();
        return reg.substr(0, reg.rfind('/')) + "/..";
    }
};

TEST_F(RadianceMeta, CardsAreRegisteredAndTheTournamentListsAreLegal) {
    for (CardDefId id : {kBilgewaterTracker, kRivenBrokenBlade, kTheEmpyrean, kAnOffer,
                         kRecklessInnovator, kBedevilingStranger, kTrifarianCaptain,
                         kBladeOfTheExile, kRakelstake, kRuinedMonastery,
                         kBattlePreparations, kTheExile}) {
        const Card* c = card_registry.get(id);
        ASSERT_NE(c, nullptr) << id;
        EXPECT_EQ(c->def().id, id);
        EXPECT_EQ(c->def().set_code, "RAD");
    }
    EXPECT_EQ(card_db.findByName("Riven, The Exile")->id, kTheExile);
    DeckValidator validator(card_db);
    for (const char* f : {"riven_exile.txt", "kaisa_void.txt", "kennen_tempest.txt"}) {
        auto deck = DeckValidator::loadFromDeckList(root() + "/decks/meta/" + f, card_db);
        auto r = validator.validate(deck);
        std::string errs;
        for (const auto& e : r.errors) errs += e + "; ";
        EXPECT_TRUE(r.is_legal) << f << ": " << errs;
    }
}

// ── "When I become exhausted" ──────────────────────────────────────────────

TEST_F(RadianceMeta, ExhaustingAReadyObjectIsAnEventButAnExhaustedOneIsNot) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    int fired = 0;
    auto conn = events.on_object_state_changed.connect([&](const ObjectStateChangedEvent& e) {
        if (e.what_changed == "exhausted") ++fired;
    });
    auto u = addUnit(P1, kInvalidId, 2);
    exec.exhaustObject(u);
    exec.exhaustObject(u);
    conn.disconnect();
    EXPECT_EQ(fired, 1);
}

// ── 992 Riven, Broken Blade ────────────────────────────────────────────────

TEST_F(RadianceMeta, Riven_FirstExhaustBuffsHer) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto riven = addUnit(P1, kRivenBrokenBlade, 0, 0);
    state.getObject(riven).is_exhausted = true;
    driveResumableTrigger(kRivenBrokenBlade, P1, riven, pickSeq({0}), exec);
    EXPECT_EQ(state.getObject(riven).buff_count, 1);
}

TEST_F(RadianceMeta, Riven_SpendsHerBuffToReadyHerselfOncePerTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto riven = addUnit(P1, kRivenBrokenBlade, 0, 0);
    auto& r = state.getObject(riven);
    r.buff_count = 1;
    r.is_exhausted = true;
    driveResumableTrigger(kRivenBrokenBlade, P1, riven, pickSeq({2}), exec);
    EXPECT_FALSE(state.getObject(riven).is_exhausted);
    EXPECT_EQ(state.getObject(riven).buff_count, 0);

    // Exhausted again the same turn: "ready me" was chosen, and with no buff
    // only "buff me" is left.
    state.getObject(riven).is_exhausted = true;
    std::vector<int> offered;
    driveResumableTrigger(kRivenBrokenBlade, P1, riven,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal) offered.push_back(i.chosen_value.value_or(-1));
            return legal.front();
        }, exec);
    EXPECT_EQ(offered, std::vector<int>{0});
    EXPECT_EQ(state.getObject(riven).buff_count, 1);
    EXPECT_TRUE(state.getObject(riven).is_exhausted);

    // A third time: buff and ready are both used; only the stun is left.
    auto enemy = addUnit(P2, kInvalidId, 3, 0);
    driveResumableTrigger(kRivenBrokenBlade, P1, riven, pickSeq({1, enemy}), exec);
    EXPECT_TRUE(state.getObject(enemy).is_stunned);
    EXPECT_EQ(state.getObject(riven).buff_count, 0);
}

TEST_F(RadianceMeta, Riven_ModesComeBackNextTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto riven = addUnit(P1, kRivenBrokenBlade, 0, 0);
    state.getObject(riven).is_exhausted = true;
    driveResumableTrigger(kRivenBrokenBlade, P1, riven, pickSeq({0}), exec);
    state.getObject(riven).buff_count = 0;
    state.turn.turn_number += 1;
    driveResumableTrigger(kRivenBrokenBlade, P1, riven, pickSeq({0}), exec);
    EXPECT_EQ(state.getObject(riven).buff_count, 1);
}

// ── 1043 Reckless Innovator ────────────────────────────────────────────────

TEST_F(RadianceMeta, RecklessInnovator_PlaysAnExhaustedGold) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto ri = addUnit(P1, kRecklessInnovator, 0, 0);
    driveResumableTrigger(kRecklessInnovator, P1, ri, pickSeq({0}), exec);
    EXPECT_EQ(goldTokens(P1), 1);
}

TEST_F(RadianceMeta, RecklessInnovator_PaysChaosToDraw) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto ri = addUnit(P1, kRecklessInnovator, 0, 0);
    addToDeck(P1, kBrutalizer);
    // No Chaos: drawing is not offered.
    std::vector<int> offered;
    driveResumableTrigger(kRecklessInnovator, P1, ri,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal) offered.push_back(i.chosen_value.value_or(-1));
            return legal.front();
        }, exec);
    EXPECT_EQ(offered, std::vector<int>{0});
    addRune(P1, Domain::Chaos);
    const size_t hand = state.player(P1).hand.size();
    driveResumableTrigger(kRecklessInnovator, P1, ri, pickSeq({1}), exec);
    EXPECT_EQ(state.player(P1).hand.size(), hand + 1);
    EXPECT_EQ(readyRuneCount(P1, Domain::Chaos), 0);   // recycled for the [C]
}

// ── 958 Bilgewater Tracker ─────────────────────────────────────────────────

TEST_F(RadianceMeta, BilgewaterTracker_DealsOneToTwoUnits) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto me = addUnit(P1, kBilgewaterTracker, 0, 0);
    auto a = addUnit(P2, kInvalidId, 3, 0);
    auto b = addUnit(P2, kInvalidId, 3, 1);
    auto mine = addUnit(P1, kInvalidId, 3, 1);
    driveResumableTrigger(kBilgewaterTracker, P1, me, pickSeq({2, a, b}), exec);   // "2 units"
    EXPECT_EQ(state.getObject(a).damage_marked, 1);
    EXPECT_EQ(state.getObject(b).damage_marked, 1);
    EXPECT_EQ(state.getObject(mine).damage_marked, 0);
}

// ── 1045 Bedeviling Stranger ───────────────────────────────────────────────

TEST_F(RadianceMeta, BedevilingStranger_ReturnsAnEnemyFromACrowdedBattlefield) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto me = addUnit(P1, kBedevilingStranger, 0, 0);
    addUnit(P1, kInvalidId, 2, 0);
    auto e1 = addUnit(P2, kInvalidId, 3, 0);
    addUnit(P2, kInvalidId, 3, 0);
    auto far = addUnit(P2, kInvalidId, 3, 1);   // only 1 unit there
    std::vector<GameObjectId> offered;
    driveResumableTrigger(kBedevilingStranger, P1, me,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal)   // "you may": yes
                if (i.chosen_objects.empty() && i.chosen_value == 1) return i;
            for (const auto& i : legal)
                if (!i.chosen_objects.empty()) offered.push_back(i.chosen_objects[0]);
            for (const auto& i : legal)
                if (!i.chosen_objects.empty() && i.chosen_objects[0] == e1) return i;
            return legal.front();
        }, exec);
    EXPECT_TRUE(std::find(offered.begin(), offered.end(), far) == offered.end());
    EXPECT_TRUE(inHand(P2, e1));
}

TEST_F(RadianceMeta, BedevilingStranger_NeedsFourUnitsThere) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto me = addUnit(P1, kBedevilingStranger, 0, 0);
    auto e1 = addUnit(P2, kInvalidId, 3, 0);
    addUnit(P2, kInvalidId, 3, 0);
    driveResumableTrigger(kBedevilingStranger, P1, me, nullptr, exec);
    EXPECT_TRUE(state.getObject(e1).location.has_value());
}

// ── 1064 Trifarian Captain ─────────────────────────────────────────────────

TEST_F(RadianceMeta, TrifarianCaptain_DrawsACheapUnitOrGearAndRecyclesTheRest) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto me = addUnit(P1, kTrifarianCaptain, 0, 0);
    auto bottom = addToDeck(P1, kSingularity);
    auto spell = addToDeck(P1, kSingularity);       // a spell: not eligible
    auto gear = addToDeck(P1, kBrutalizer);         // 2-cost gear: eligible
    auto big = addToDeck(P1, kTheEmpyrean);         // 8-cost unit: not eligible
    auto top = addToDeck(P1, kSingularity);
    std::vector<GameObjectId> offered;
    driveResumableTrigger(kTrifarianCaptain, P1, me,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal)   // "you may reveal": yes
                if (i.chosen_objects.empty() && i.chosen_value == 1) return i;
            for (const auto& i : legal)
                if (!i.chosen_objects.empty()) offered.push_back(i.chosen_objects[0]);
            return legal.front();
        }, exec);
    EXPECT_TRUE(inHand(P1, gear));

    EXPECT_TRUE(offered.empty() || offered == std::vector<GameObjectId>{gear});
    // The bottom card (5th) was not looked at and is now above the recycled 3.
    const auto& deck = state.player(P1).main_deck;
    ASSERT_EQ(deck.size(), 4u);
    EXPECT_EQ(deck.back(), bottom);
    for (auto c : {spell, big, top}) EXPECT_TRUE(inDeck(P1, c));
}

// ── 1082 Blade of the Exile ────────────────────────────────────────────────

TEST_F(RadianceMeta, BladeOfTheExile_AttachesWhenPaidByExhausting) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    Card* blade = card_registry.get(kBladeOfTheExile);
    ASSERT_TRUE(blade->equipByExhaustingAUnit());
    auto gear = addGear(P1, kBladeOfTheExile);
    state.getObject(gear).might_bonus = 3;
    auto unit = addUnit(P1, kInvalidId, 2, 0);
    CardContext ctx{state, events, exec, P1, gear};
    EXPECT_TRUE(blade->onEquipPaid(ctx, unit));
    EXPECT_EQ(state.getObject(gear).attached_to, unit);
    EXPECT_EQ(state.getObject(unit).current_might, 5);
    auto enemy = addUnit(P2, kInvalidId, 2, 0);
    auto gear2 = addGear(P1, kBladeOfTheExile);
    CardContext ctx2{state, events, exec, P1, gear2};
    EXPECT_FALSE(blade->onEquipPaid(ctx2, enemy));
}

// ── 1087 The Exile ─────────────────────────────────────────────────────────

TEST_F(RadianceMeta, TheExile_UnitsPlayedToControlledBattlefieldsEnterReady) {
    const Card* exile = card_registry.get(kTheExile);
    state.battlefields[0].controller = P1;
    auto at_mine = addUnit(P1, kInvalidId, 2, 0);
    auto at_open = addUnit(P1, kInvalidId, 2, 1);
    auto at_base = addUnit(P1, kInvalidId, 2);
    EXPECT_TRUE(exile->grantsEnterReadyTo(state, kInvalidId, state.getObject(at_mine)));
    EXPECT_FALSE(exile->grantsEnterReadyTo(state, kInvalidId, state.getObject(at_open)));
    EXPECT_FALSE(exile->grantsEnterReadyTo(state, kInvalidId, state.getObject(at_base)));
}

// ── 1041 An Offer You Can't Refuse ─────────────────────────────────────────

TEST_F(RadianceMeta, AnOffer_KillsAGearAndItsControllerGetsAGold) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto gear = addGear(P2, kBrutalizer);
    auto spell = addToHand(P1, kAnOffer);
    driveResumable(kAnOffer, P1, spell, pickSeq({gear}), exec);
    EXPECT_FALSE(state.getObject(gear).location.has_value());
    EXPECT_EQ(goldTokens(P2), 1);
    EXPECT_EQ(goldTokens(P1), 0);
}

// ── 1100 Battle Preparations ───────────────────────────────────────────────

TEST_F(RadianceMeta, BattlePreparations_ReadiesTwoRunesWhenAGearCanBeShownOff) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto unit = addUnit(P1, kInvalidId, 2, 0);
    for (int i = 0; i < 3; ++i) addRune(P1, Domain::Calm, /*exhausted=*/true);
    auto spell = addToHand(P1, kBattlePreparations);
    driveResumable(kBattlePreparations, P1, spell, pickSeq({unit}), exec);
    EXPECT_EQ(state.getObject(unit).current_might, 4);
    EXPECT_EQ(readyRuneCount(P1), 0);   // nothing to show off

    addToHand(P1, kBrutalizer);
    auto spell2 = addToHand(P1, kBattlePreparations);
    driveResumable(kBattlePreparations, P1, spell2, pickSeq({unit}), exec);
    EXPECT_EQ(state.getObject(unit).current_might, 6);
    EXPECT_EQ(readyRuneCount(P1), 2);
}

// ── 1006 The Empyrean ──────────────────────────────────────────────────────

TEST_F(RadianceMeta, TheEmpyrean_DrawsTwo) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto me = addUnit(P1, kTheEmpyrean, 0, 0);
    addToHand(P1, kBrutalizer);
    for (int i = 0; i < 4; ++i) addToDeck(P1, kSingularity);
    const size_t hand = state.player(P1).hand.size();
    driveResumableTrigger(kTheEmpyrean, P1, me, nullptr, exec);
    EXPECT_EQ(state.player(P1).hand.size(), hand + 2);
}

// ── 1097 Rakelstake ────────────────────────────────────────────────────────

TEST_F(RadianceMeta, Rakelstake_OncePerPlayerPerTurn) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto bf_obj = state.createObject();
    state.getObject(bf_obj).card_def_id = kRakelstake;
    state.battlefields[0].card_object_id = bf_obj;
    int predicted = 0;
    auto conn = events.on_log.connect([&](const LogEvent& e) {
        if (e.message.find("RAKELSTAKE") != std::string::npos) ++predicted;
    });
    fireTriggerAs(kRakelstake, P1, bf_obj, TriggerType::WhenYouPlayAUnit, exec);
    fireTriggerAs(kRakelstake, P1, bf_obj, TriggerType::WhenYouPlayAUnit, exec);
    fireTriggerAs(kRakelstake, P2, bf_obj, TriggerType::WhenYouPlayAUnit, exec);
    state.turn.turn_number += 1;
    fireTriggerAs(kRakelstake, P1, bf_obj, TriggerType::WhenYouPlayAUnit, exec);
    conn.disconnect();
    EXPECT_EQ(predicted, 3);
}

// ── 1098 Ruined Monastery ──────────────────────────────────────────────────

TEST_F(RadianceMeta, RuinedMonastery_DiscardsOneAndChannelsAnExhaustedRune) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto bf_obj = state.createObject();
    state.getObject(bf_obj).card_def_id = kRuinedMonastery;
    auto card = addToHand(P1, kBrutalizer);
    // One rune in the rune deck.
    auto rune = state.createObject();
    {
        auto& r = state.getObject(rune);
        r.owner = r.controller = P1;
        r.card_type = CardType::Rune;
        r.domains = {Domain::Calm};
        r.zone = ZoneType::RuneDeck;
        state.player(P1).rune_deck.push_back(rune);
    }
    driveResumableTrigger(kRuinedMonastery, P1, bf_obj,
        [&](const std::vector<Intent>& legal) {
            for (const auto& i : legal)
                if (i.chosen_value && *i.chosen_value == 1) return i;   // yes
            for (const auto& i : legal)
                if (!i.chosen_objects.empty() && i.chosen_objects[0] == card) return i;
            return legal.front();
        }, exec);
    EXPECT_TRUE(inTrash(P1, card));
    EXPECT_TRUE(state.getObject(rune).location.has_value());
    EXPECT_TRUE(state.getObject(rune).is_exhausted);
}

}  // namespace
}  // namespace riftbound::test
