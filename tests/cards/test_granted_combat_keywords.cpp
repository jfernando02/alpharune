// [Assault] and [Shield] given by an aura or an Equipment add Might while the unit is an
// attacker / defender, like printed ones. They used to set only the keyword, so e.g. Serrated
// Dirk's [Assault 2] or Captain Farron's [Assault] did nothing in combat. One ability gives one
// instance, even when it is recorded twice; instances from different sources add up.
// Conditional keywords ("[Level 6] I have ...", "While I'm [Mighty] ...") are not printed ones.
// Found by the card audit (docs/rules-audit/cards.csv).

#include "tests/cards/card_test_fixture.h"

namespace riftbound::test {
namespace {

class GrantedCombatKeywordsTest : public CardTestFixture {
protected:
    static GameObject::AuraEffect grant(GameObjectId source, Keyword k, int value) {
        GameObject::AuraEffect ae;
        ae.source = source;
        ae.keyword = k;
        ae.keyword_value = value;
        return ae;
    }
};

TEST_F(GrantedCombatKeywordsTest, AuraAssaultAddsMightWhileAttacking) {
    auto u = addUnit(P1, kInvalidId, /*might=*/3, /*at_bf=*/0);
    auto& o = state.getObject(u);
    o.aura_effects.push_back(grant(/*source=*/101, Keyword::Assault, 2));
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 3) << "not an attacker";
    o.combat_designation = CombatDesignation::Attacker;
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 5);
}

TEST_F(GrantedCombatKeywordsTest, OneSourceGivesOneInstanceDifferentSourcesAdd) {
    auto u = addUnit(P1, kInvalidId, /*might=*/3, /*at_bf=*/0);
    auto& o = state.getObject(u);
    o.combat_designation = CombatDesignation::Attacker;
    o.aura_effects.push_back(grant(101, Keyword::Assault, 1));
    o.aura_effects.push_back(grant(101, Keyword::Assault, 1));   // the same ability, recorded twice
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 4);
    o.aura_effects.push_back(grant(102, Keyword::Assault, 0));   // another source, Assault (1)
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 5);
}

TEST_F(GrantedCombatKeywordsTest, AuraShieldAddsMightWhileDefending) {
    auto u = addUnit(P1, kInvalidId, /*might=*/2, /*at_bf=*/0);
    auto& o = state.getObject(u);
    o.aura_effects.push_back(grant(101, Keyword::Shield, 1));
    o.combat_designation = CombatDesignation::Attacker;
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 2) << "Shield counts only while defending";
    o.combat_designation = CombatDesignation::Defender;
    o.recomputeMight();
    EXPECT_EQ(o.current_might, 3);
}

// Cards whose keywords are only conditional or only given to other units must not have them
// printed: a printed keyword is always on.
TEST_F(GrantedCombatKeywordsTest, ConditionalKeywordsAreNotPrinted) {
    struct Case { CardDefId id; Keyword k; };
    const Case cases[] = {
        {232, Keyword::Deflect}, {232, Keyword::Ganking}, {232, Keyword::Shield},  // Fiora, Victorious
        {675, Keyword::Deflect}, {675, Keyword::Ganking},                          // Master Yi, Tempered
        {637, Keyword::Ganking},                                                   // Gustwalker
        {464, Keyword::Ganking},                                                   // Sivir, Mercenary
        {609, Keyword::Deflect},                                                   // Mosstomper
        {715, Keyword::Deflect},                                                   // Carrion Dredger (its token)
        {15, Keyword::Assault},                                                    // Captain Farron ("other")
        {349, Keyword::Assault},                                                   // Rumble (his own aura)
        {452, Keyword::Assault},                                                   // Ancient Warmonger
        {620, Keyword::Tank},                                                      // Lillia (token units)
        {638, Keyword::Temporary},                                                 // Petal Pixie
        {63, Keyword::Deflect},                                                    // Spirit's Refuge (gear)
        {426, Keyword::Deflect},                                                   // Petricite Monument (gear)
    };
    for (const auto& c : cases) {
        const auto& def = card_db.get(c.id);
        EXPECT_FALSE(def.keywords.has(c.k)) << def.name << " prints " << toString(c.k);
    }
    for (CardDefId id : {15, 349, 452}) EXPECT_EQ(card_db.get(id).assault_value, 0) << card_db.get(id).name;
    for (CardDefId id : {232}) EXPECT_EQ(card_db.get(id).shield_value, 0) << card_db.get(id).name;
    for (CardDefId id : {232, 675}) EXPECT_EQ(card_db.get(id).deflect_value, 0) << card_db.get(id).name;
}

// Petal Pixie: "I have +1 [M] for each of your units with [Temporary] at my battlefield." It is
// not [Temporary] itself, so it survives its controller's Beginning Phase.
TEST_F(GrantedCombatKeywordsTest, PetalPixieIsNotTemporary) {
    auto pixie = addUnit(P1, 638, /*might=*/1, /*at_bf=*/0);
    EXPECT_FALSE(state.getObject(pixie).hasKeyword(Keyword::Temporary));
}

}  // namespace
}  // namespace riftbound::test
