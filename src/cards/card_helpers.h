#pragma once
/// @file card_helpers.h
/// Cross-cutting card helpers used across multiple card types. Type-specific
/// bases live in their type dirs (gear/equip_base.h, units/weaponmaster_base.h).
#include "cards/card.h"
#include "cards/card_registry.h"
#include "core/game_state.h"
#include "core/events.h"
#include "engine/effect_executor.h"
#include "cards/gear/equip_base.h"   // canonical standardEquip(+energy,Domain) + bases
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace riftbound {

// The engine-known Gold gear token's card_def_id.
constexpr CardDefId kGoldGearCardDefId = 326;

// ── One Power of `domain` (any domain if nullopt) ──
// Paid the way CR 164.2 lets it be: restricted Power `purpose` may spend
// (RunePool::restricted; none for a plain "pay [C]" trigger cost), floating
// Power of that domain, universal Power, then recycling a matching rune — ready
// or exhausted (an exhausted one first, since a ready rune can still be
// exhausted for Energy). Equip costs go through payEquipCost (equip_base.h).
inline bool runeMatchesDomain(const GameObject& rune, std::optional<Domain> domain) {
    if (!domain.has_value()) return true;
    for (auto d : rune.domains) if (d == *domain) return true;
    return false;
}
inline bool payableOnePower(const GameState& state, PlayerId player,
                            std::optional<Domain> domain, SpendPurpose purpose = {}) {
    const auto& pool = state.player(player).rune_pool;
    if (pool.universal_power > 0 || pool.restrictedPowerFor(purpose) > 0) return true;
    if (domain.has_value()) { if (pool.power[static_cast<int>(*domain)] > 0) return true; }
    else if (pool.totalPower() > 0) return true;
    const LocationId base{BaseLocation{player}};
    for (const auto& [id, obj] : state.objects)
        if (obj.isRune() && obj.controller == player && obj.location.has_value() &&
            *obj.location == base && runeMatchesDomain(obj, domain)) return true;
    return false;
}
/// Recycle `rune` (in `player`'s base) to pay Power: it goes to the bottom of
/// the rune deck and "you recycled a rune" triggers see it (LeftBoardEvent).
inline void recycleRuneForPower(CardContext& ctx, PlayerId player, GameObjectId rune) {
    auto& state = ctx.state;
    auto& dr = state.getObject(rune);
    const LocationId was_at = dr.location.value_or(LocationId{BaseLocation{player}});
    dr.location = std::nullopt;
    dr.zone = ZoneType::RuneDeck;
    dr.is_exhausted = false;
    state.player(player).rune_deck.insert(state.player(player).rune_deck.begin(), rune);
    state.player(player).power_spent_this_turn++;
    ctx.events.logTrace("  ADDL_COST: recycled " + dr.name + " for power");
    ctx.events.emit(LeftBoardEvent{rune, player, CardType::Rune, was_at, ZoneType::RuneDeck, false});
}
inline bool payOnePower(CardContext& ctx, PlayerId player,
                        std::optional<Domain> domain, SpendPurpose purpose = {}) {
    auto& state = ctx.state;
    auto& pool = state.player(player).rune_pool;
    {
        int need = 1;
        pool.spendRestrictedPower(purpose, need);
        if (need == 0) return true;
    }
    if (domain.has_value()) {
        int& own = pool.power[static_cast<int>(*domain)];
        if (own > 0) { --own; return true; }
    } else {
        for (int di = 0; di < static_cast<int>(Domain::Count); ++di)
            if (pool.power[di] > 0) { --pool.power[di]; return true; }
    }
    if (pool.universal_power > 0) { --pool.universal_power; return true; }
    const LocationId base_loc{BaseLocation{player}};
    GameObjectId rune = kInvalidId;
    for (auto& [id, obj] : state.objects) {
        if (!obj.isRune() || obj.controller != player) continue;
        if (!obj.location.has_value() || *obj.location != base_loc) continue;
        if (!runeMatchesDomain(obj, domain)) continue;
        if (rune == kInvalidId || (obj.is_exhausted && !state.getObject(rune).is_exhausted))
            rune = id;
    }
    if (rune == kInvalidId) return false;
    recycleRuneForPower(ctx, player, rune);
    return true;
}

// ── Battlefields and combat ──
/// The battlefield whose card object is `card_object` (a battlefield card's
/// own ctx.source, or a conquer / hold trigger's subject on a legend).
inline std::optional<BattlefieldId> battlefieldOfCard(const GameState& state,
                                                      GameObjectId card_object) {
    if (card_object == kInvalidId) return std::nullopt;
    for (const auto& bf : state.battlefields)
        if (bf.card_object_id == card_object) return bf.id;
    return std::nullopt;
}
/// "Here" for an ability of `source`: the battlefield it is at, or for a
/// battlefield card, that battlefield. nullopt when it is not at one.
inline std::optional<BattlefieldId> battlefieldHere(const GameState& state,
                                                    GameObjectId source) {
    if (!state.objectExists(source)) return std::nullopt;
    if (auto bf = battlefieldOfCard(state, source)) return bf;
    return state.getObject(source).battlefieldId();
}
/// Keeps the objects at `source`'s battlefield ("an enemy unit here").
inline std::vector<GameObjectId> keepHere(const GameState& state, GameObjectId source,
                                          std::vector<GameObjectId> ids) {
    const auto here = battlefieldHere(state, source);
    std::vector<GameObjectId> out;
    if (!here) return out;
    for (auto id : ids)
        if (state.objectExists(id) && state.getObject(id).battlefieldId() == here)
            out.push_back(id);
    return out;
}
/// Drops `source` itself ("another unit").
inline std::vector<GameObjectId> dropSource(GameObjectId source, std::vector<GameObjectId> ids) {
    ids.erase(std::remove(ids.begin(), ids.end(), source), ids.end());
    return ids;
}
/// Combat damage `player` assigned beyond lethal in their combat at `bf` this
/// turn — 0 if their most recent combat was somewhere else (or there was none).
inline int excessCombatDamageAt(const GameState& state, PlayerId player, BattlefieldId bf) {
    const auto& ps = state.player(player);
    return ps.last_combat_bf == bf ? ps.excess_combat_damage : 0;
}

// Equip [C] / [A] with no Energy: the canonical payment in gear/equip_base.h.
inline bool standardEquip(CardContext& ctx, GameObjectId gear_id,
                          GameObjectId unit_id, std::optional<Domain> domain) {
    return standardEquip(ctx, gear_id, unit_id, /*energy_cost=*/0, domain);
}

// Attach a gear directly (no cost) to a unit (e.g. Edge of Night's
// play-from-facedown → attach). Detaches from any prior bearer first.
inline void attachFree(CardContext& ctx, GameObjectId gear_id, GameObjectId unit_id) {
    auto& state = ctx.state;
    auto& gear = state.getObject(gear_id);
    auto& unit = state.getObject(unit_id);
    if (gear.attached_to.has_value() && state.objectExists(*gear.attached_to)) {
        auto& old = state.getObject(*gear.attached_to);
        old.attachment_might_bonus -= gear.might_bonus;
        auto it = std::find(old.attachments.begin(), old.attachments.end(), gear_id);
        if (it != old.attachments.end()) old.attachments.erase(it);
        old.recomputeMight();
    }
    gear.attached_to = unit_id;
    unit.attachments.push_back(gear_id);
    gear.location = unit.location;
    gear.zone = unit.zone;
    unit.attachment_might_bonus += gear.might_bonus;
    unit.recomputeMight();
    ctx.events.logTrace("ATTACH (free): " + gear.name + " -> " + unit.name);
    ctx.events.emit(ObjectStateChangedEvent{gear_id, "attached"});
    ctx.events.emit(ObjectStateChangedEvent{unit_id, "equipped"});
}

// ── Discard-then-act resumable helpers ──
// discardOneThenAct: discard exactly one (agent-chosen), then post_fn(ctx, discarded).
template <typename PostFn>
inline void discardOneThenAct(CardContext& ctx, const std::string& label,
                              PostFn&& post_fn) {
    auto& ri = ctx.state.chain.resuming.value();
    auto& ps = ctx.state.player(ctx.controller);
    auto build_choices = [&]() {
        std::vector<Intent> out;
        for (auto cid : ps.hand) {
            Intent c; c.type = IntentType::MakeChoice; c.player = ctx.controller;
            c.chosen_objects = {cid}; out.push_back(std::move(c));
        }
        return out;
    };
    switch (ri.resume_point) {
    case 0:
        if (ps.hand.empty()) { post_fn(ctx, kInvalidId); return; }
        ctx.executor.requestChoice(ctx.controller, build_choices(), label);
        ri.resume_point = 1;
        return;
    case 1: {
        GameObjectId chosen = kInvalidId;
        auto choice = ctx.executor.takeChoice();
        if (choice && !choice->chosen_objects.empty()) {
            chosen = choice->chosen_objects[0];
            ctx.executor.applyDiscard(ctx.controller, chosen);
        }
        post_fn(ctx, chosen);
        return;
    }
    }
}

// discardThenAct: discard up to `count` (agent-chosen), then post_fn(ctx).
template <typename PostFn>
inline void discardThenAct(CardContext& ctx, int count,
                           const std::string& label, PostFn&& post_fn) {
    auto& ri = ctx.state.chain.resuming.value();
    auto& ps = ctx.state.player(ctx.controller);
    auto build_choices = [&]() {
        std::vector<Intent> out;
        for (auto cid : ps.hand) {
            Intent c; c.type = IntentType::MakeChoice; c.player = ctx.controller;
            c.chosen_objects = {cid}; out.push_back(std::move(c));
        }
        return out;
    };
    switch (ri.resume_point) {
    case 0:
        if (ps.hand.empty() || count <= 0) { post_fn(ctx); return; }
        ri.resume_data = {count};
        ctx.executor.requestChoice(ctx.controller, build_choices(), label);
        ri.resume_point = 1;
        return;
    case 1: {
        auto choice = ctx.executor.takeChoice();
        if (choice && !choice->chosen_objects.empty())
            ctx.executor.applyDiscard(ctx.controller, choice->chosen_objects[0]);
        if (!ri.resume_data.empty()) ri.resume_data[0]--;
        if (ri.resume_data.empty() || ri.resume_data[0] <= 0 || ps.hand.empty()) {
            post_fn(ctx); return;
        }
        ctx.executor.requestChoice(ctx.controller, build_choices(), label);
        return;
    }
    }
}

// ── Countering (CR 426) ──
// CR 420.4.b: non-triggered checks of "cards you've played" count FINALIZED
// cards, and a countered card was finalized — so countering does not undo the
// played count. CR 420.4.a.1 / 426.1.b: abilities that trigger on a card being
// played simply never trigger, because those fire at resolution. Nothing is
// reverted here; the function remains as the single place that would change.
inline void revertCounteredPlay(CardContext& /*ctx*/, const ChainItem& /*top*/) {}

/// Put a card that was removed from the chain without resolving where it
/// belongs: `dest` (trash — CR 426.1.a.1 — or hand for "return it to hand"),
/// unless it carries a "then banish it" replacement (CR 391.3.a: a Flow spell
/// that would leave the chain other than by its own execution is banished).
inline void disposeCounteredCard(CardContext& ctx, GameObjectId id,
                                 ZoneType dest = ZoneType::Trash) {
    if (!ctx.state.objectExists(id)) return;
    auto& obj = ctx.state.getObject(id);
    obj.location = std::nullopt;
    if (obj.banish_on_leave_chain) {
        obj.banish_on_leave_chain = false;
        obj.zone = ZoneType::Banishment;
        ctx.state.player(obj.owner).banishment.push_back(id);
        ctx.state.player(obj.owner).cards_banished_this_turn++;
        ctx.events.logTrace("COUNTER: " + obj.name + " -> banished (played for Flow)");
        ctx.events.emit(ObjectStateChangedEvent{id, "banished_owned"});
        return;
    }
    obj.zone = dest;
    if (dest == ZoneType::Hand) ctx.state.player(obj.owner).hand.push_back(id);
    else ctx.state.player(obj.owner).trash.push_back(id);
}

// "This can't be countered" (the spell's own card) or "Your spells and
// abilities can't be countered" (its controller). Every counter effect checks
// this before removing a chain item.
inline bool isUncounterable(const CardContext& ctx, const ChainItem& item) {
    if (ctx.state.player(item.controller).spells_cant_be_countered) return true;
    if (ctx.registry && ctx.state.objectExists(item.source)) {
        const Card* c = ctx.registry->get(ctx.state.getObject(item.source).card_def_id);
        if (c && c->cantBeCountered()) return true;
    }
    return false;
}

inline void counterChainTop(CardContext& ctx) {
    if (ctx.state.chain.items.empty()) return;
    auto& top = ctx.state.chain.items.back();
    if (!top.is_spell) return;
    if (isUncounterable(ctx, top)) return;
    auto countered = top.source;
    revertCounteredPlay(ctx, top);  // CR 425.1.b
    ctx.state.chain.items.pop_back();
    if (ctx.state.objectExists(countered)) {
        ctx.events.logTrace("COUNTER: " + ctx.state.getObject(countered).name + " countered");
        disposeCounteredCard(ctx, countered);
    }
}

// ── Gold tokens ──
// `player` (default: the effect's controller) plays the Gold token exhausted.
inline GameObjectId createGoldExhausted(CardContext& ctx, PlayerId player = PlayerId::None) {
    if (player == PlayerId::None) player = ctx.controller;
    LocationId loc{BaseLocation{player}};
    auto tok = ctx.executor.createToken(player, CardType::Gear, "Gold",
                                        0, {}, {}, loc, /*enter_ready=*/false);
    if (ctx.state.objectExists(tok)) {
        auto& gold = ctx.state.getObject(tok);
        gold.is_exhausted = true;
        // Bind the Gold token card so its "[Reaction] Kill this, [E]: Add" ability exists.
        gold.card_def_id = kGoldGearCardDefId;
    }
    return tok;
}

// "Play a Gold gear token exhausted." -- every card that plays Gold says exhausted.
inline void createGoldToken(CardContext& ctx) { createGoldExhausted(ctx); }

// ── Misc ──
inline GameObjectId findMostRecentlyPlayedSpell(const GameState& state, PlayerId player) {
    const auto& ps = state.player(player);
    for (auto it = ps.trash.rbegin(); it != ps.trash.rend(); ++it) {
        if (!state.objectExists(*it)) continue;
        if (state.getObject(*it).isSpell()) return *it;
    }
    return kInvalidId;
}

inline void moveToLocation(EffectExecutor& exec, GameObjectId obj,
                           const std::optional<LocationId>& loc) {
    if (!loc) return;
    if (std::holds_alternative<BattlefieldLocation>(*loc))
        exec.moveToBattlefield(obj, std::get<BattlefieldLocation>(*loc).id);
    else
        exec.moveToBase(obj);
}

// An additional cost chosen with pickTarget / pickTargetPair ("exhaust a
// friendly unit", "return a friendly unit") is paid at finalization, and the
// helper checks the choice again as the item resolves, when the paid object
// no longer fits the list it was chosen from. Pass that list through this:
// once the cost is paid, every object is accepted.
inline std::vector<GameObjectId> costChoices(const CardContext& ctx,
                                             std::vector<GameObjectId> choices) {
    if (!Card::costAlreadyPaid(ctx)) return choices;
    std::vector<GameObjectId> all;
    all.reserve(ctx.state.objects.size());
    for (const auto& [id, obj] : ctx.state.objects) all.push_back(id);
    return all;
}

inline bool isMighty(const GameObject& obj) { return obj.current_might >= 5; }

inline bool hasTag(const GameObject& obj, const std::string& tag) {
    for (const auto& t : obj.tags) if (t == tag) return true;
    return false;
}

inline bool isEquipment(const GameObject& obj) {
    return obj.isGear() && hasTag(obj, "Equipment");
}

// ── Tag presence (Ivern / Bird-Cat-Dog-Poro family) ──
struct TagPresence {
    bool bird = false, cat = false, dog = false, poro = false;
    int count() const { return (int)bird + (int)cat + (int)dog + (int)poro; }
    bool allFour() const { return bird && cat && dog && poro; }
};

inline TagPresence scanFriendlyTags(const GameState& state, PlayerId controller) {
    TagPresence out;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || obj.controller != controller) continue;
        if (!obj.location.has_value()) continue;
        for (const auto& tag : obj.tags) {
            if      (tag == "Bird") out.bird = true;
            else if (tag == "Cat")  out.cat  = true;
            else if (tag == "Dog")  out.dog  = true;
            else if (tag == "Poro") out.poro = true;
        }
    }
    return out;
}

// ── Empower costs ───────────────────────────────────────────────────────────
// "[Empower] [N]" / "[Empower] [N][R]" / "[Empower] [N][A][A]".
inline Card::EmpowerSpec empowerCost(int energy, int power = 0,
                                     Domain power_domain = Domain::Fury,
                                     bool any_domain = false) {
    Card::EmpowerSpec s;
    s.valid = true;
    s.cost.energy = energy;
    s.cost.power = power;
    s.cost.power_domain = power_domain;
    s.cost.power_any_domain = any_domain;
    return s;
}

// ── Flow costs ──────────────────────────────────────────────────────────────
inline Card::FlowCost flow(int energy, int power = 0,
                           Domain power_domain = Domain::Fury,
                           bool any_domain = false) {
    Card::FlowCost f;
    f.valid = true;
    f.energy = energy;
    f.power = power;
    f.power_domain = power_domain;
    f.any_domain = any_domain;
    return f;
}

// ── Passive-aura plumbing ───────────────────────────────────────────────────
// Card::applyPassiveAura is invoked once per on-board instance of the card but
// is not told WHICH instance, so implementations loop over every instance. A
// marker effect makes each instance get its effect once per aura pass no
// matter how many copies are in play.
constexpr int kAuraOnceMarker = -777;

template <typename Fn>
void forEachSelfOnce(const Card& card, GameState& state, PlayerId controller, Fn&& fn) {
    for (auto& [sid, self] : state.objects) {
        if (self.card_def_id != card.cardDefId() || self.controller != controller) continue;
        if (!self.location.has_value() && self.zone != ZoneType::LegendZone) continue;
        bool seen = false;
        for (const auto& ae : self.aura_effects)
            if (ae.source == sid && ae.keyword == Keyword::Count &&
                ae.keyword_value == kAuraOnceMarker) { seen = true; break; }
        if (seen) continue;
        GameObject::AuraEffect marker;
        marker.source = sid;
        marker.keyword_value = kAuraOnceMarker;
        self.aura_effects.push_back(marker);
        fn(sid, self);
    }
}

/// "[Empowered][>] ..." — visit each Empowered instance once per aura pass.
template <typename Fn>
void forEachEmpoweredSelf(const Card& card, GameState& state, PlayerId controller, Fn&& fn) {
    forEachSelfOnce(card, state, controller, [&](GameObjectId sid, GameObject& self) {
        if (self.isEmpowered()) fn(sid, self);
    });
}

/// "+N [M]" from a continuous effect.
inline void auraMight(GameObject& obj, GameObjectId source, int amount) {
    GameObject::AuraEffect ae;
    ae.source = source;
    ae.might_bonus = amount;
    obj.aura_effects.push_back(ae);
}

/// A keyword from a continuous effect. [Assault N] / [Shield N] also carry
/// their conditional Might (while attacking / defending), as the engine's
/// aura fold only sums might_bonus.
inline void auraKeyword(GameObject& obj, GameObjectId source, Keyword kw, int value = 0) {
    GameObject::AuraEffect ae;
    ae.source = source;
    ae.keyword = kw;
    ae.keyword_value = value;
    if (kw == Keyword::Assault && obj.combat_designation == CombatDesignation::Attacker)
        ae.might_bonus = value > 0 ? value : 1;
    if (kw == Keyword::Shield && obj.combat_designation == CombatDesignation::Defender)
        ae.might_bonus = value > 0 ? value : 1;
    obj.aura_effects.push_back(ae);
}

// ── Paying Energy inside an effect ("you may pay [N] to ...") ───────────────
/// Energy `player` could pay right now: floating pool + ready runes in base.
inline int payableEnergy(const GameState& state, PlayerId player) {
    int n = state.player(player).rune_pool.energy;
    for (const auto& [id, obj] : state.objects)
        if (obj.isRune() && obj.controller == player && !obj.is_exhausted &&
            obj.location.has_value()) ++n;
    return n;
}

/// Pay `amount` Energy: floating pool first, then exhaust ready runes. Returns
/// false (paying nothing) if it can't be covered.
inline bool payEnergy(CardContext& ctx, PlayerId player, int amount) {
    if (amount <= 0) return true;
    if (payableEnergy(ctx.state, player) < amount) return false;
    auto& pool = ctx.state.player(player).rune_pool;
    int from_pool = std::min(pool.energy, amount);
    pool.energy -= from_pool;
    amount -= from_pool;
    for (auto& [id, obj] : ctx.state.objects) {
        if (amount <= 0) break;
        if (!obj.isRune() || obj.controller != player || obj.is_exhausted) continue;
        if (!obj.location.has_value()) continue;
        obj.is_exhausted = true;
        ctx.events.emit(ObjectStateChangedEvent{id, "exhausted"});
        --amount;
    }
    return true;
}

// ── Board queries ───────────────────────────────────────────────────────────
/// Runes `player` controls (in base, ready or exhausted).
inline int runesControlled(const GameState& state, PlayerId player) {
    int n = 0;
    for (const auto& [id, obj] : state.objects)
        if (obj.isRune() && obj.controller == player && obj.location.has_value()) ++n;
    return n;
}

/// Gear `player` controls on the board.
inline int gearControlled(const GameState& state, PlayerId player) {
    int n = 0;
    for (const auto& [id, obj] : state.objects)
        if (obj.isGear() && obj.controller == player && obj.location.has_value()) ++n;
    return n;
}

/// Cards in `player`'s trash with the given name.
inline int namedInTrash(const GameState& state, PlayerId player, const std::string& name) {
    int n = 0;
    for (auto cid : state.player(player).trash)
        if (state.objectExists(cid) && state.getObject(cid).name == name) ++n;
    return n;
}

/// On-board units matching `pred`, honoring "can't be chosen by enemy spells
/// and abilities" for units `chooser` doesn't control.
template <typename Pred>
std::vector<GameObjectId> chooseableUnits(const GameState& state, PlayerId chooser, Pred&& pred) {
    std::vector<GameObjectId> out;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        if (obj.controller != chooser && obj.untargetable_by_enemy) continue;
        if (pred(obj)) out.push_back(id);
    }
    return out;
}

/// Other units `controller` has at the same location as `self`.
inline int otherFriendlyUnitsHere(const GameState& state, const GameObject& self) {
    if (!self.location.has_value()) return 0;
    int n = 0;
    for (const auto& [id, obj] : state.objects) {
        if (id == self.id || !obj.isUnit() || obj.controller != self.controller) continue;
        if (obj.location.has_value() && *obj.location == *self.location) ++n;
    }
    return n;
}

// ── Agent choices by hand ──
// For cards whose resolution asks a custom question (a unit from a computed
// list, then a destination, …) and so build their own MakeChoice options
// rather than going through Card::pickTarget.

/// A MakeChoice option that picks object `id`.
inline Intent objChoice(PlayerId p, GameObjectId id) {
    Intent c;
    c.type = IntentType::MakeChoice;
    c.player = p;
    c.chosen_objects = {id};
    return c;
}
/// The object the agent picked from objChoice options (kInvalidId if none).
inline GameObjectId takenObject(CardContext& ctx) {
    auto c = ctx.executor.takeChoice();
    return (c && !c->chosen_objects.empty()) ? c->chosen_objects[0] : kInvalidId;
}

/// A MakeChoice option carrying the int `v`, shown to the agent as `label`.
inline Intent valChoice(PlayerId p, int v, const std::string& label) {
    Intent c;
    c.type = IntentType::MakeChoice;
    c.player = p;
    c.chosen_value = v;
    c.choice_label = label;
    return c;
}
/// The int the agent picked from valChoice options (`fallback` if none).
inline int takenValue(CardContext& ctx, int fallback) {
    auto c = ctx.executor.takeChoice();
    return (c && c->chosen_value.has_value()) ? *c->chosen_value : fallback;
}

/// Destinations a card effect may move `u` to, int-coded for valChoice:
/// 0 = its controller's base, 1 + i = battlefield i. Excludes where it is now.
inline std::vector<int> moveDests(const GameState& state, const GameObject& u) {
    std::vector<int> out;
    if (!u.location.has_value()) return out;
    if (!u.isAtBase()) out.push_back(0);
    for (const auto& bf : state.battlefields) {
        auto here = u.battlefieldId();
        if (here.has_value() && *here == bf.id) continue;
        out.push_back(1 + static_cast<int>(bf.id));
    }
    return out;
}
inline std::string destLabel(int d) {
    return d <= 0 ? std::string("Base") : "Battlefield #" + std::to_string(d - 1);
}
/// Move `u` to a moveDests destination.
inline void moveTo(CardContext& ctx, GameObjectId u, int dest) {
    if (dest <= 0) ctx.executor.moveToBase(u);
    else ctx.executor.moveToBattlefield(u, static_cast<BattlefieldId>(dest - 1));
}


// ── Radiance tokens and keywords ────────────────────────────────────────────
/// The Bomb gear token (CR 187.12): [Deploy] and "[Deathknell][>] Deal 2 to an
/// enemy unit here and kill all other Bombs you control here."
constexpr CardDefId kBombTokenCardDefId = 1086;

/// "Play a Bomb gear token to `bf`."
inline GameObjectId createBomb(CardContext& ctx, BattlefieldId bf,
                               PlayerId player = PlayerId::None) {
    if (player == PlayerId::None) player = ctx.controller;
    auto tok = ctx.executor.createToken(player, CardType::Gear, "Bomb", 0, {"Bomb"}, {},
                                        LocationId{BattlefieldLocation{bf}}, /*enter_ready=*/true);
    if (ctx.state.objectExists(tok)) {
        auto& b = ctx.state.getObject(tok);
        b.card_def_id = kBombTokenCardDefId;
        b.deploy = true;

        b.is_exhausted = false;
    }
    return tok;
}

/// "Play a 1 [M] Recruit unit token to `loc`."
inline GameObjectId createRecruit(CardContext& ctx, LocationId loc, bool ready = false,
                                  PlayerId player = PlayerId::None) {
    if (player == PlayerId::None) player = ctx.controller;
    return ctx.executor.createToken(player, CardType::Unit, "Recruit", 1, {"Recruit"},
                                    KeywordSet{}, loc, ready);
}
inline GameObjectId createRecruitAtBase(CardContext& ctx, PlayerId player = PlayerId::None) {
    if (player == PlayerId::None) player = ctx.controller;
    return createRecruit(ctx, LocationId{BaseLocation{player}}, false, player);
}

/// "Play a 3 [M] Mech unit token to `loc`."
inline GameObjectId createMech(CardContext& ctx, LocationId loc, bool ready = false) {
    return ctx.executor.createToken(ctx.controller, CardType::Unit, "Mech", 3, {"Mech"},
                                    KeywordSet{}, loc, ready);
}

/// Where `source` is ("here"): its location, or its controller's base.
inline LocationId locationHere(const GameState& state, GameObjectId source, PlayerId controller) {
    if (state.objectExists(source)) {
        const auto& o = state.getObject(source);
        if (o.location.has_value()) return *o.location;
        if (o.last_known.valid && o.last_known.location.has_value()) return *o.last_known.location;
    }
    return LocationId{BaseLocation{controller}};
}

/// Units at battlefield `bf` not controlled by `chooser` that `chooser` may choose.
inline std::vector<GameObjectId> enemyUnitsAt(const GameState& state, PlayerId chooser,
                                              std::optional<BattlefieldId> bf) {
    std::vector<GameObjectId> out;
    if (!bf) return out;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || obj.controller == chooser || obj.untargetable_by_enemy) continue;
        if (obj.battlefieldId() == bf) out.push_back(id);
    }
    return out;
}

/// Battlefield index (as BattlefieldId) chosen with pickMode: -1 waiting,
/// -2 no legal battlefield. `ok` filters the battlefields offered.
template <typename Ok>
inline int pickBattlefield(Card& self, CardContext& ctx, const std::string& label, Ok&& ok) {
    const int n = static_cast<int>(ctx.state.battlefields.size());
    if (n == 0) return -2;
    uint32_t legal = 0;
    std::vector<std::string> labels;
    for (int i = 0; i < n && i < 32; ++i) {
        labels.push_back("Battlefield #" + std::to_string(i));
        if (ok(ctx.state.battlefields[i].id)) legal |= (1u << i);
    }
    const int m = self.pickMode(ctx, label, n, labels, legal);
    if (m < 0) return m;
    return static_cast<int>(ctx.state.battlefields[m].id);
}
inline int pickBattlefield(Card& self, CardContext& ctx, const std::string& label) {
    return pickBattlefield(self, ctx, label, [](BattlefieldId) { return true; });
}

/// [Disarm X] (CR 831): "When I attack, give an enemy unit here -X [M] this
/// turn." Called from the unit's WhenIAttack trigger (`unit` = the attacker).
/// Returns false while the choice is pending or being finalized.
inline bool resolveDisarm(Card& self, CardContext& ctx, GameObjectId unit, int x) {
    std::optional<BattlefieldId> here;
    if (ctx.state.objectExists(unit)) here = ctx.state.getObject(unit).battlefieldId();
    const auto t = self.pickTarget(ctx, "Disarm: an enemy unit here",
                                   enemyUnitsAt(ctx.state, ctx.controller, here));
    if (ctx.executor.hasPendingChoice()) return false;
    if (Card::finalizingChoices(ctx)) return false;
    if (t == kInvalidId || !ctx.state.objectExists(t) || x <= 0) return true;
    ctx.executor.giveTemporaryMight(t, -x);
    ctx.events.logTrace("DISARM " + std::to_string(x) + ": " + ctx.state.getObject(t).name +
                        " -" + std::to_string(x) + " [M] this turn");
    return true;
}

/// Deal `amount` to `target` and kill it if that is lethal.
inline void dealAndKill(CardContext& ctx, GameObjectId target, int amount) {
    if (amount <= 0 || !ctx.state.objectExists(target)) return;
    ctx.executor.dealDamage(target, amount, ctx.source);
    if (ctx.state.objectExists(target) && ctx.state.getObject(target).location.has_value() &&
        ctx.state.getObject(target).hasLethalDamage())
        ctx.executor.killObject(target);
}

/// The Energy cost printed on `obj`'s card (0 for tokens).
inline int printedEnergy(const CardContext& ctx, const GameObject& obj) {
    return obj.card_def_id == kInvalidId ? 0 : ctx.executor.cardDB().get(obj.card_def_id).energy_cost;
}

/// "When you play me, play a Bomb gear token to a battlefield." (the whole
/// trigger body; the battlefield is chosen at finalization).
inline void playBombTrigger(Card& self, CardContext& ctx, const std::string& who) {
    const int bf = pickBattlefield(self, ctx, who + ": play a Bomb to a battlefield");
    if (bf == -1) return;   // waiting for the agent
    if (Card::finalizingChoices(ctx)) return;
    if (bf < 0) return;
    createBomb(ctx, static_cast<BattlefieldId>(bf));
    ctx.events.logTrace(who + ": played a Bomb to battlefield #" + std::to_string(bf));
}

/// Units at battlefield `bf` that `chooser` may choose (enemy ones that can't
/// be chosen are left out).
inline std::vector<GameObjectId> unitsAtBattlefield(const GameState& state, PlayerId chooser,
                                                    BattlefieldId bf) {
    std::vector<GameObjectId> out;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isUnit() || obj.battlefieldId() != bf) continue;
        if (obj.controller != chooser && obj.untargetable_by_enemy) continue;
        out.push_back(id);
    }
    return out;
}

/// Every unit on the board `chooser` may choose.
inline std::vector<GameObjectId> anyUnits(const GameState& state, PlayerId chooser) {
    return chooseableUnits(state, chooser, [](const GameObject&) { return true; });
}
inline std::vector<GameObjectId> friendlyUnits(const GameState& state, PlayerId chooser) {
    return chooseableUnits(state, chooser,
                           [&](const GameObject& o) { return o.controller == chooser; });
}
inline std::vector<GameObjectId> enemyUnits(const GameState& state, PlayerId chooser) {
    return chooseableUnits(state, chooser,
                           [&](const GameObject& o) { return o.controller != chooser; });
}
/// Gear on the board (`friendly_only`: `chooser`'s) that `chooser` may choose.
inline std::vector<GameObjectId> boardGear(const GameState& state, PlayerId chooser,
                                           bool friendly_only = false) {
    std::vector<GameObjectId> out;
    for (const auto& [id, obj] : state.objects) {
        if (!obj.isGear() || !obj.location.has_value()) continue;
        if (friendly_only && obj.controller != chooser) continue;
        if (obj.controller != chooser && obj.untargetable_by_enemy) continue;
        out.push_back(id);
    }
    return out;
}
/// The chain item's triggering subject (kInvalidId if none).
inline GameObjectId triggerSubject(const CardContext& ctx) {
    return ctx.state.chain.resuming ? ctx.state.chain.resuming->triggering_subject : kInvalidId;
}
/// Kill the units in `ids` that have lethal damage (after an AoE).
inline void killLethal(CardContext& ctx, const std::vector<GameObjectId>& ids) {
    for (auto id : ids)
        if (ctx.state.objectExists(id) && ctx.state.getObject(id).location.has_value() &&
            ctx.state.getObject(id).isUnit() && ctx.state.getObject(id).hasLethalDamage())
            ctx.executor.killObject(id);
}

/// A unit's total [Shield]: printed/temporary value plus one instance per
/// aura source that grants it (as GameObject::recomputeMight counts them).
inline int totalShield(const GameState& state, GameObjectId id) {
    if (!state.objectExists(id)) return 0;
    const auto& o = state.getObject(id);
    int v = o.shield_value;
    std::vector<GameObjectId> seen;
    for (const auto& ae : o.aura_effects) {
        if (ae.keyword != Keyword::Shield) continue;
        if (std::find(seen.begin(), seen.end(), ae.source) != seen.end()) continue;
        seen.push_back(ae.source);
        v += ae.keyword_value > 0 ? ae.keyword_value : 1;
    }
    if (v == 0 && o.hasKeyword(Keyword::Shield)) v = 1;
    return v;
}

/// The battlefield a battlefield card object stands for.
inline std::optional<BattlefieldId> bfOfCard(const GameState& state, GameObjectId card) {
    for (const auto& bf : state.battlefields)
        if (bf.card_object_id == card) return bf.id;
    return std::nullopt;
}

/// Friendly units of `p` at battlefield `bf`.
inline std::vector<GameObjectId> friendlyUnitsAt(const GameState& state, PlayerId p,
                                                 std::optional<BattlefieldId> bf) {
    std::vector<GameObjectId> out;
    if (!bf) return out;
    for (const auto& [id, o] : state.objects)
        if (o.isUnit() && o.controller == p && o.battlefieldId() == bf) out.push_back(id);
    return out;
}

/// Take `card` out of its owner's hand / trash / banishment list (before an
/// effect plays it from there).
inline void pullFromZoneLists(GameState& state, GameObjectId card) {
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        auto& ps = state.player(pid);
        for (auto* v : {&ps.hand, &ps.trash, &ps.banishment})
            v->erase(std::remove(v->begin(), v->end(), card), v->end());
    }
}

/// "When one or more [friendly / enemy] units die": true only for the first
/// death of `owner`'s units in a death batch (units that die at the same time,
/// GameState::death_batch), so the ability triggers once per batch. Use it in
/// Card::triggerCondition with the event's `subject`.
inline bool firstDeathInBatch(const GameState& state, PlayerId owner, GameObjectId subject) {
    if (!state.objectExists(subject)) return true;
    const auto& lk = state.getObject(subject).last_known;
    if (!lk.valid) return true;
    for (const auto& [id, o] : state.objects) {
        if (id == subject || !o.isUnit() || o.controller != owner || o.location.has_value()) continue;
        if (o.last_known.valid && o.last_known.death_batch == lk.death_batch) return false;
    }
    return true;
}

} // namespace riftbound
