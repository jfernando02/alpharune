/// @file test_radiance_reksai.cpp
/// The two Radiance cards the Rek'Sai tournament list (decks/meta/
/// reksai_breacher.txt) needed beyond the engine's earlier cards:
/// Lingering Animus and Hunting Dog.

#include "tests/cards/card_test_fixture.h"

#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "engine/chain_manager.h"
#include "engine/effect_executor.h"
#include "engine/trigger_manager.h"
#include "rules/deck_validator.h"

#include <gtest/gtest.h>

#include <string>

namespace riftbound::test {
namespace {

constexpr CardDefId kLingeringAnimus = 960;
constexpr CardDefId kHuntingDog      = 1074;

class RadianceReksai : public CardTestFixture {
protected:
    std::string root() {
        auto reg = findRegistryPath();
        return reg.substr(0, reg.rfind('/')) + "/..";
    }
};

TEST_F(RadianceReksai, CardsAreRegisteredAndTheListIsLegal) {
    for (CardDefId id : {kLingeringAnimus, kHuntingDog}) {
        const Card* c = card_registry.get(id);
        ASSERT_NE(c, nullptr) << id;
        EXPECT_EQ(c->def().set_code, "RAD");
    }
    DeckValidator validator(card_db);
    auto deck = DeckValidator::loadFromDeckList(root() + "/decks/meta/reksai_breacher.txt", card_db);
    auto r = validator.validate(deck);
    std::string errs;
    for (const auto& e : r.errors) errs += e + "; ";
    EXPECT_TRUE(r.is_legal) << errs;
}

TEST_F(RadianceReksai, LingeringAnimus_EntersReadyOnlyAfterAFriendlyDeath) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    const Card* animus = card_registry.get(kLingeringAnimus);
    EXPECT_FALSE(animus->entersReadyOnPlay(state, P1));
    exec.killObject(addUnit(P2, kInvalidId, 2));          // an enemy death does not count
    EXPECT_FALSE(animus->entersReadyOnPlay(state, P1));
    exec.killObject(addUnit(P1, kInvalidId, 2));
    EXPECT_TRUE(animus->entersReadyOnPlay(state, P1));
    state.player(P1).resetTurnTracking();
    EXPECT_FALSE(animus->entersReadyOnPlay(state, P1));
}

TEST_F(RadianceReksai, LingeringAnimus_AFriendlyGearDyingCountsToo) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto gear = addUnit(P1, kInvalidId);
    state.getObject(gear).card_type = CardType::Gear;
    exec.killObject(gear);
    EXPECT_TRUE(card_registry.get(kLingeringAnimus)->entersReadyOnPlay(state, P1));
}

// An effect that plays the unit (Rek'Sai's legend, Void Burrower) goes
// through EffectExecutor::playIgnoringCost, not the engine's resolvePermanent.
TEST_F(RadianceReksai, LingeringAnimus_EntersReadyWhenAnEffectPlaysIt) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    auto before = addToHand(P1, kLingeringAnimus);
    exec.playIgnoringCost(P1, before);
    EXPECT_TRUE(state.getObject(before).is_exhausted);
    exec.killObject(addUnit(P1, kInvalidId, 2));
    auto after = addToHand(P1, kLingeringAnimus);
    exec.playIgnoringCost(P1, after);
    EXPECT_FALSE(state.getObject(after).is_exhausted);
}

TEST_F(RadianceReksai, HuntingDog_DeathknellPlaysADeflectBirdToBase) {
    EffectExecutor exec(state, events, card_db, &card_registry);
    ChainManager cm(state, events, card_db);
    TriggerManager tm(state, events, card_db, cm, card_registry);
    tm.setEffectExecutor(&exec);
    tm.subscribe();
    auto dog = addUnit(P1, kHuntingDog, 3, /*at_bf=*/0);
    exec.killObject(dog);                                  // dies at a battlefield
    ASSERT_EQ(state.chain.items.size(), 1u) << "the Deathknell goes on the chain";
    EXPECT_EQ(state.chain.items.back().source, dog);
    CardContext ctx{state, events, exec, P1, dog};
    card_registry.get(kHuntingDog)->onTrigger(ctx, {});
    int birds = 0;
    for (const auto& [id, o] : state.objects)
        if (o.isToken() && o.controller == P1 && o.name == "Bird" && o.location.has_value()) {
            ++birds;
            EXPECT_TRUE(std::holds_alternative<BaseLocation>(*o.location));
            EXPECT_TRUE(o.hasKeyword(Keyword::Deflect));
            EXPECT_EQ(o.current_might, 1);
        }
    EXPECT_EQ(birds, 1);
}

}  // namespace
}  // namespace riftbound::test
