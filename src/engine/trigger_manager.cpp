#include "trigger_manager.h"
#include "cards/card.h"
#include "engine/effect_executor.h"

namespace riftbound {

TriggerManager::TriggerManager(GameState& state, EventBus& events,
                               const CardDB& card_db, ChainManager& chain,
                               const CardRegistry& card_registry)
    : state_(state), events_(events), card_db_(card_db), chain_(chain),
      card_registry_(card_registry) {}

void TriggerManager::subscribe() {
    connections_.push_back(events_.on_spell_resolved.connect(
        [this](const SpellResolvedEvent& e) { onSpellResolved(e); }));
    connections_.push_back(events_.on_card_played.connect(
        [this](const CardPlayedEvent& e) { onCardPlayed(e); }));
    connections_.push_back(events_.on_entered_board.connect(
        [this](const EnteredBoardEvent& e) { onEnteredBoard(e); }));
    connections_.push_back(events_.on_token_created.connect(
        [this](const TokenCreatedEvent& e) { onTokenCreated(e); }));
    // Delayed "the next time it takes damage" abilities, scoped to the unit.
    connections_.push_back(events_.on_damage_dealt.connect(
        [this](const DamageDealtEvent& e) {
            if (e.amount <= 0) return;
            checkDelayedAbilities(TriggerType::WhenItTakesDamage, PlayerId::Player1, e.target);
            checkDelayedAbilities(TriggerType::WhenItTakesDamage, PlayerId::Player2, e.target);
        }));
    connections_.push_back(events_.on_combat_started.connect(
        [this](const CombatStartedEvent& e) { onCombatStarted(e); }));
    connections_.push_back(events_.on_combat_ended.connect(
        [this](const CombatEndedEvent& e) { onCombatEnded(e); }));
    connections_.push_back(events_.on_unit_died.connect(
        [this](const UnitDiedEvent& e) { onUnitDied(e); }));
    connections_.push_back(events_.on_score.connect(
        [this](const ScoreEvent& e) { onScore(e); }));
    connections_.push_back(events_.on_unit_moved.connect(
        [this](const UnitMovedEvent& e) { onUnitMoved(e); }));
    connections_.push_back(events_.on_phase_changed.connect(
        [this](const PhaseChangedEvent& e) { onPhaseChanged(e); }));
    connections_.push_back(events_.on_unit_stunned.connect(
        [this](const UnitStunnedEvent& e) { onUnitStunned(e); }));
    connections_.push_back(events_.on_unit_readied.connect(
        [this](const UnitReadiedEvent& e) { onUnitReadied(e); }));
    connections_.push_back(events_.on_card_hidden.connect(
        [this](const CardHiddenEvent& e) { onCardHidden(e); }));
    connections_.push_back(events_.on_played_from_facedown.connect(
        [this](const PlayedFromFacedownEvent& e) { onPlayedFromFacedown(e); }));
    connections_.push_back(events_.on_unit_returned_to_hand.connect(
        [this](const UnitReturnedToHandEvent& e) { onUnitReturnedToHand(e); }));
    connections_.push_back(events_.on_showdown_started.connect(
        [this](const ShowdownStartedEvent& e) { onShowdownStarted(e); }));
    connections_.push_back(events_.on_card_revealed.connect(
        [this](const CardRevealedEvent& e) { onCardRevealed(e); }));
    connections_.push_back(events_.on_object_state_changed.connect(
        [this](const ObjectStateChangedEvent& e) { onObjectStateChanged(e); }));
    connections_.push_back(events_.on_cards_drawn.connect(
        [this](const CardsDrawnEvent& e) { onCardsDrawn(e); }));
    connections_.push_back(events_.on_left_board.connect(
        [this](const LeftBoardEvent& e) { onLeftBoard(e); }));
}

TriggerManager::~TriggerManager() {
    for (auto& c : connections_) {
        if (c.connected()) c.disconnect();
    }
}

void TriggerManager::onUnitStunned(const UnitStunnedEvent& e) {
    // WhenYouStun fires for cards controlled by the stunner when the
    // stunned unit's controller is the opposite side (you can't trigger
    // "stun an enemy" by stunning your own unit). Mirrors the pattern in
    // onUnitDied / onCombatEnded.
    if (e.stunner == e.victim_controller) return;  // self-stun, not "enemy"
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != e.stunner) continue;
        if (obj.card_def_id == kInvalidId) continue;
        if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
        const Card* card = card_registry_.get(obj.card_def_id);
        if (!card) continue;
        if (card->triggerType() != TriggerType::WhenYouStun) continue;
        events_.logTrace("TRIGGER: " + obj.name + " WhenYouStun (stunned=" +
                         std::to_string(e.stunned_unit) + ")");
        // Capture the stunned-unit id into card_counters so the trigger's
        // onTrigger can read it post-resolution (combat designations get
        // cleared by the time the chain item resolves — same pattern as
        // __defend_attacker_id).
        obj.card_counters["__stunned_unit_id"] =
            static_cast<int>(e.stunned_unit);
        chain_.addAbility(id, e.stunner, obj.card_def_id);
    }
    checkDelayedAbilities(TriggerType::WhenYouStun, e.stunner, e.stunned_unit);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Delayed abilities (CR 389-392)
// ═══════════════════════════════════════════════════════════════════════════════

void TriggerManager::checkDelayedAbilities(TriggerType trigger,
                                            PlayerId relevant_player,
                                            GameObjectId event_object) {
    for (auto& da : state_.delayed_abilities) {
        if (da.fired) continue;
        if (da.trigger != trigger) continue;
        if (da.controller != relevant_player) continue;
        // Object-scoped delayed abilities (e.g. Deadly Flourish: "when IT
        // dies") fire only for the specific target. kInvalidId = unscoped.
        if (da.target_filter != kInvalidId &&
            da.target_filter != event_object) continue;

        da.fired = true;
        events_.logTrace("DELAYED_ABILITY: firing for " +
                         (state_.objectExists(da.source) ? state_.getObject(da.source).name : "?"));
        chain_.addAbility(da.source, da.controller, da.card_def_id);
        // Stamp the originating trigger so the card's onTrigger can branch on
        // ctx.firing_trigger (e.g. Grim Resolve's WhenIWinCombat XP rider,
        // Targon's Peak's AtEndOfTurn ready-runes).
        if (!state_.chain.items.empty()) {
            state_.chain.items.back().fired_trigger = trigger;
            const GameObjectId subject = event_object != kInvalidId ? event_object : da.payload;
            if (subject != kInvalidId) state_.chain.items.back().triggering_subject = subject;
        }
    }

    // Remove fired abilities
    state_.delayed_abilities.erase(
        std::remove_if(state_.delayed_abilities.begin(), state_.delayed_abilities.end(),
            [](const DelayedAbility& da) { return da.fired; }),
        state_.delayed_abilities.end());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Equipment triggers — check attached gear when unit events fire
// ═══════════════════════════════════════════════════════════════════════════════

void TriggerManager::fireEquippedTriggers(GameObjectId unit_id, PlayerId controller,
                                           TriggerType trigger) {
    if (state_.game_over || !state_.objectExists(unit_id)) return;
    auto& unit = state_.getObject(unit_id);

    for (auto gear_id : unit.attachments) {
        if (!state_.objectExists(gear_id)) continue;
        auto& gear = state_.getObject(gear_id);

        Card* gear_card = card_registry_.get(gear.card_def_id);
        if (!gear_card) continue;
        if (gear_card->equippedTriggerType() != trigger) continue;

        events_.logTrace("EQUIP_TRIGGER: " + gear.name + " fires on " +
                         unit.name + " (" + toString(controller) + ")");

        if (effect_executor_) {
            CardContext ctx{state_, events_, *effect_executor_, controller, gear_id};
            gear_card->onEquippedTrigger(ctx, unit_id, {});
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helper — get trigger type from Card object
// ═══════════════════════════════════════════════════════════════════════════════

static TriggerType getTrigger(const CardRegistry& reg, CardDefId def_id) {
    Card* card = reg.get(def_id);
    return card ? card->triggerType() : TriggerType::None;
}

// Multi-trigger-aware membership test: true if the card declares `t` among
// its trigger types. Use this (not `getTrigger(...) == t`) so cards that
// register more than one trigger fire on each.
static bool cardFiresOn(const CardRegistry& reg, CardDefId def_id, TriggerType t) {
    Card* card = reg.get(def_id);
    return card && card->firesOn(t);
}

void TriggerManager::fireBattlefieldTriggers(TriggerType t, PlayerId player,
                                              BattlefieldId at_battlefield,
                                              GameObjectId subject) {
    for (auto& bf : state_.battlefields) {
        if (at_battlefield != kInvalidId && bf.id != at_battlefield) continue;
        if (!state_.objectExists(bf.card_object_id)) continue;
        if (cardFiresOn(card_registry_, state_.getObject(bf.card_object_id).card_def_id, t)) {
            fireTrigger(bf.card_object_id, player, 0, t, subject);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helper
// ═══════════════════════════════════════════════════════════════════════════════

// Legend cards live in PlayerState::legend_zone, not on the board, so they
// have no `location` and are skipped by every state_.objects iteration that
// gates on it. The card text on legends frequently uses "When you …"
// (player-scoped) phrasing but the engine encodes those as WhenIConquerOrHold
// / WhenIWinCombat / WhenYouPlayASpell on the legend's Card subclass. This
// helper sweeps both players' legend zones and fires the trigger if the
// legend's declared TriggerType matches `t`. `relevant_player` controls
// scoping — for events that only fire for one player (score, combat win),
// pass that player's id; pass PlayerId::None to sweep both legends (e.g.
// "when YOU play a spell" fires only on your legend, but we still need to
// check both legend zones since either player may be the one acting).
void TriggerManager::fireLegendTrigger(TriggerType t, PlayerId relevant_player,
                                        int triggering_spell_energy_spent,
                                        GameObjectId subject) {
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        if (relevant_player != PlayerId::None && pid != relevant_player) continue;
        for (auto legend_id : state_.player(pid).legends()) {
            if (!state_.objectExists(legend_id)) continue;
            auto& legend = state_.getObject(legend_id);
            if (cardFiresOn(card_registry_, legend.card_def_id, t))
                fireTrigger(legend_id, pid, triggering_spell_energy_spent, t, subject);
        }
    }
}

void TriggerManager::fireTrigger(GameObjectId source, PlayerId controller,
                                  int triggering_spell_energy_spent,
                                  TriggerType which,
                                  GameObjectId subject) {
    if (!state_.objectExists(source)) return;
    auto& obj = state_.getObject(source);

    // Skip triggers on gear with inactive rules text (CR 718.2)
    if (obj.is_rules_text_inactive) return;

    // LeBlanc, Everywhere at Once (652): "your [Temporary] effects at my
    // battlefield don't trigger." If this firing source is a friendly
    // [Temporary] unit at a battlefield, and a same-controller card there
    // declares it suppresses Temporary triggers, skip firing.
    if (obj.isUnit() && obj.hasKeyword(Keyword::Temporary) && obj.location.has_value()) {
        if (auto bf = obj.battlefieldId()) {
            for (auto& [oid, other] : state_.objects) {
                if (oid == source) continue;
                if (other.controller != obj.controller) continue;
                auto obf = other.battlefieldId();
                if (!obf || *obf != *bf) continue;
                Card* oc = card_registry_.get(other.card_def_id);
                if (oc && oc->suppressesTemporaryTriggersHere()) {
                    events_.logTrace("TRIGGER_SUPPRESSED: " + obj.name +
                                     " (Temporary) at LeBlanc's battlefield");
                    return;
                }
            }
        }
    }

    // For single-trigger cards `which` may be None; resolve it from the
    // card so multi-trigger cards still record which event fired.
    TriggerType fired = which;
    if (fired == TriggerType::None) {
        fired = getTrigger(card_registry_, obj.card_def_id);
    }
    if (fired == TriggerType::None) return;

    // The card's intervening condition ("When a BUFFED friendly unit dies"):
    // if it isn't met the ability does not trigger.
    if (const Card* card = card_registry_.get(obj.card_def_id);
        card && !card->triggerCondition(state_, source, controller, fired, subject))
        return;

    events_.logDebug("TRIGGER: " + obj.name + " (" + toString(controller) +
                     ") fires ability");
    chain_.addAbility(source, controller, obj.card_def_id);
    // Snapshot the triggering spell's spent value on the just-added
    // chain item so resume-time reads aren't subject to clobber by a
    // subsequent spell-play. Phase 6q+ engine-audit follow-on for
    // Virtuoso / Forgotten Library correctness.
    if (!state_.chain.items.empty()) {
        state_.chain.items.back().fired_trigger = fired;
        if (triggering_spell_energy_spent > 0) {
            state_.chain.items.back().triggering_spell_energy_spent =
                triggering_spell_energy_spent;
        }
        if (subject != kInvalidId) {
            state_.chain.items.back().triggering_subject = subject;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Event handlers
// ═══════════════════════════════════════════════════════════════════════════════

void TriggerManager::fireTrashPlayWatchers(PlayerId player, GameObjectId played) {
    if (!state_.objectExists(played)) return;
    const auto def = state_.getObject(played).card_def_id;
    state_.turn.event_card_energy = def == kInvalidId ? 0 : card_db_.get(def).energy_cost;
    const auto trash = state_.player(player).trash;   // copy: firing may not move cards, but be safe
    for (auto tid : trash) {
        if (tid == played || !state_.objectExists(tid)) continue;
        if (cardFiresOn(card_registry_, state_.getObject(tid).card_def_id,
                        TriggerType::WhenYouPlayACardInTrash))
            fireTrigger(tid, player, 0, TriggerType::WhenYouPlayACardInTrash, played);
    }
    state_.turn.event_card_energy = 0;
}

void TriggerManager::onCardPlayed(const CardPlayedEvent& e) {
    if (!state_.objectExists(e.object)) return;
    auto& obj = state_.getObject(e.object);


    // Per-turn "what have you played" tracking (Swain, Visionary).
    {
        auto& pps = state_.player(e.player);
        if (e.card_type == CardType::Spell) { pps.played_spell_this_turn = true; pps.spells_played_this_turn++; }
        if (e.card_type == CardType::Unit && !obj.isToken()) pps.played_nontoken_unit_this_turn = true;
        if (e.card_type == CardType::Gear && !obj.isToken()) pps.played_nontoken_gear_this_turn = true;
    }
    // CR 420.4.a: abilities that trigger on a card being played trigger when
    // playing it is COMPLETED by its resolution. A permanent resolves as it is
    // finalized, so its play triggers fire here; a spell's fire from
    // onSpellResolved (and never, if it is countered — 420.4.a.1 / 426.1.b).
    if (e.card_type == CardType::Spell)
        obj.card_counters["__energy_spent_on_play"] = e.energy_spent;

    // "When you play a card" (any type) on the player's other cards + legend,
    // and "When an opponent plays a gear" on the other player's cards.
    if (e.card_type != CardType::Spell) {
        std::vector<std::pair<GameObjectId, TriggerType>> fires;
        for (auto& [id, other] : state_.objects) {
            if (id == e.object || other.card_def_id == kInvalidId) continue;
            if (!other.location.has_value() && other.zone != ZoneType::LegendZone) continue;
            if (other.controller == e.player &&
                cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouPlayACard))
                fires.push_back({id, TriggerType::WhenYouPlayACard});
            if (other.controller != e.player && e.card_type == CardType::Gear &&
                cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenOpponentPlaysAGear))
                fires.push_back({id, TriggerType::WhenOpponentPlaysAGear});
        }
        for (auto& [id, t] : fires)
            fireTrigger(id, state_.getObject(id).controller, e.energy_spent, t,
                        /*subject=*/e.object);
        fireTrashPlayWatchers(e.player, e.object);
    }

    // Vision keyword: "When you play me, look at top card, may recycle it"
    // (CR 817.1.b). The look is private (CardRevealedEvent to the player);
    // the player answers "recycle it?" as the card is played. Vision may be
    // printed or granted by a friendly card on the board (grantsKeywordTo).
    bool has_vision = obj.hasKeyword(Keyword::Vision);
    if (!has_vision && obj.isPermanent())
        for (const auto& [gid, g] : state_.objects) {
            if (gid == e.object || g.controller != e.player || !g.location.has_value()) continue;
            if (const Card* gc = card_registry_.get(g.card_def_id);
                gc && gc->grantsKeywordTo(state_, gid, obj, Keyword::Vision)) { has_vision = true; break; }
        }
    if (has_vision && obj.isPermanent()) {
        auto& ps = state_.player(e.player);
        if (!ps.main_deck.empty()) {
            auto top_card = ps.main_deck.back();
            auto& top_obj = state_.getObject(top_card);
            // Reveal the peeked card to the controller only (private).
            events_.emit(CardRevealedEvent{
                top_card, top_obj.card_def_id, top_obj.owner,
                /*revealed_to_all=*/false, /*revealed_to=*/e.player,
                ZoneType::MainDeck});
            const bool recycle = effect_executor_
                ? effect_executor_->chooseObjectOrNoneNow(e.player, {top_card}) == top_card
                : top_obj.isSpell();
            if (recycle) {
                ps.main_deck.pop_back();
                ps.main_deck.insert(ps.main_deck.begin(), top_card);
            }
        }
    }

    // "When you play me" — fire on the card that was just played
    if (obj.isPermanent()) {
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouPlayMe)) {
            fireTrigger(e.object, e.player, 0, TriggerType::WhenYouPlayMe);
        }
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::AsYouPlayMe)) {
            fireTrigger(e.object, e.player, 0, TriggerType::AsYouPlayMe);
        }
    }

    // "The next N times you play a unit this turn, draw 1." (Fanfare)
    if (e.card_type == CardType::Unit && effect_executor_) {
        auto& dps = state_.player(e.player);
        if (dps.draw_on_unit_play_this_turn > 0) {
            dps.draw_on_unit_play_this_turn--;
            effect_executor_->drawCards(e.player, 1);
        }
    }

    // "When you play a unit" / "When you play a spell" — fire on OTHER objects
    for (auto& [id, other] : state_.objects) {
        if (!other.location.has_value()) continue;
        if (other.controller != e.player) continue;
        if (id == e.object) continue;

        if (e.card_type == CardType::Unit &&
            cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouPlayAUnit)) {
            fireTrigger(id, e.player, 0, TriggerType::WhenYouPlayAUnit, /*subject=*/e.object);
        }
        if (e.card_type == CardType::Gear &&
            cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouPlayAGear)) {
            fireTrigger(id, e.player, 0, TriggerType::WhenYouPlayAGear, /*subject=*/e.object);
        }
    }

    // Battlefield cards with "When you play a unit here" (e.g. Star Spring).
    // Fire only on the battlefield the just-played unit landed at.
    if (e.card_type == CardType::Unit && state_.objectExists(e.object)) {
        auto played_bf = state_.getObject(e.object).battlefieldId();
        if (played_bf) {
            fireBattlefieldTriggers(TriggerType::WhenYouPlayAUnit, e.player, *played_bf);
        }
    }

    // "When an opponent plays a unit" — fire on the OPPONENT's on-board cards
    // (Vex, Apathetic: gated "while I'm at a battlefield"; stuns the played
    // unit). The played unit's id is captured into the watcher's card_counters
    // so its onTrigger can act on it (triggers receive empty targets).
    if (e.card_type == CardType::Unit) {
        PlayerId opp = opponent(e.player);
        for (auto& [id, other] : state_.objects) {
            if (other.controller != opp || !other.location.has_value()) continue;
            if (cardFiresOn(card_registry_, other.card_def_id,
                            TriggerType::WhenOpponentPlaysAUnit)) {
                other.card_counters["__opp_played_unit_id"] = static_cast<int>(e.object);
                fireTrigger(id, opp, 0, TriggerType::WhenOpponentPlaysAUnit);
            }
        }
    }

    // Legend-zone sweep: legends say "When you play a spell/unit," — the
    // legend itself has no location, so the loop above skips it. Fire any
    // matching legend on the acting player's side.
    if (e.card_type == CardType::Unit) {
        fireLegendTrigger(TriggerType::WhenYouPlayAUnit, e.player, 0, e.object);
        checkDelayedAbilities(TriggerType::WhenYouPlayAUnit, e.player, e.object);

    }
    if (e.card_type == CardType::Gear) {
        fireLegendTrigger(TriggerType::WhenYouPlayAGear, e.player, 0, e.object);
    }
}

void TriggerManager::onSpellResolved(const SpellResolvedEvent& e) {
    // CR 420.4.a: "When you play a spell" / "When you play a card" trigger now,
    // when the spell has resolved.
    if (!state_.objectExists(e.spell)) return;
    const auto& spell = state_.getObject(e.spell);
    int energy_spent = 0;
    if (auto it = spell.card_counters.find("__energy_spent_on_play");
        it != spell.card_counters.end()) energy_spent = it->second;

    std::vector<std::pair<GameObjectId, TriggerType>> fires;
    for (auto& [id, other] : state_.objects) {
        if (id == e.spell || other.controller != e.controller) continue;
        if (!other.location.has_value() && other.zone != ZoneType::LegendZone) continue;
        if (other.card_def_id == kInvalidId) continue;
        if (cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouPlayASpell))
            fires.push_back({id, TriggerType::WhenYouPlayASpell});
        if (cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouPlayACard))
            fires.push_back({id, TriggerType::WhenYouPlayACard});
    }
    for (auto& [id, t] : fires)
        fireTrigger(id, e.controller, energy_spent, t, /*subject=*/e.spell);
    fireTrashPlayWatchers(e.controller, e.spell);

    // Battlefield cards ("When a player plays a spell, they ..." — Abandoned
    // Hall; Forgotten Library). A battlefield's card object is not at a
    // location, so the sweep above never sees it. The player who played the
    // spell controls the ability (CR 190.6.c); a card whose text is limited to
    // the battlefield's controller checks that itself.
    for (auto& bf : state_.battlefields) {
        if (!state_.objectExists(bf.card_object_id)) continue;
        if (cardFiresOn(card_registry_, state_.getObject(bf.card_object_id).card_def_id,
                        TriggerType::WhenYouPlayASpell))
            fireTrigger(bf.card_object_id, e.controller, energy_spent,
                        TriggerType::WhenYouPlayASpell, /*subject=*/e.spell);
    }
}

void TriggerManager::onEnteredBoard(const EnteredBoardEvent& e) {
    if (!e.from_play) return;
    if (!state_.objectExists(e.object)) return;
    auto& obj = state_.getObject(e.object);

    if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouPlayThis)) {
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenYouPlayThis);
    }
}

// Token units are played by the effect that makes them ("play a 1 [M] Recruit
// unit token"): "When you play a token unit" watchers of its controller.
void TriggerManager::onTokenCreated(const TokenCreatedEvent& e) {
    if (e.card_type != CardType::Unit) return;
    std::vector<GameObjectId> watchers;
    for (auto& [id, obj] : state_.objects) {
        if (id == e.object || obj.controller != e.controller || !obj.location.has_value()) continue;
        if (obj.card_def_id == kInvalidId) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouPlayATokenUnit))
            watchers.push_back(id);
    }
    for (auto wid : watchers) fireTrigger(wid, e.controller, 0, TriggerType::WhenYouPlayATokenUnit, e.object);
}

void TriggerManager::onCombatStarted(const CombatStartedEvent& e) {
    auto bf_loc = BattlefieldLocation{e.battlefield};

    // Battlefield cards: "When combat starts here," (Threshold of the Gray).
    fireBattlefieldTriggers(TriggerType::WhenCombatStartsHere, e.attacker, e.battlefield);
    // "When you defend here," (Fortified Position, Reaver's Row, Ravenbloom
    // Conservatory): the defending player's ability.
    if (e.defender != PlayerId::None)
        fireBattlefieldTriggers(TriggerType::WhenYouDefendHere, e.defender, e.battlefield);

    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.isAtBattlefield()) continue;
        auto unit_bf = obj.battlefieldId();
        if (!unit_bf || *unit_bf != e.battlefield) continue;

        bool is_attacker = (obj.controller == e.attacker);
        bool is_defender = (obj.controller == e.defender);

        if (is_attacker &&
            cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIAttack)) {
            fireTrigger(id, obj.controller, 0, TriggerType::WhenIAttack);
        }
        if (is_defender &&
            cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIDefend)) {
            // Capture the current attacker UNIT (not just attacker player)
            // onto the defender's card_counters BEFORE adding the ability
            // to the chain. By the time the chain resolves the ability,
            // `combat_designation` may have been cleared (per combat
            // resolution flow), so a card that wants to act on "the
            // attacker that triggered me" can't scan the board — it has
            // to read this captured ID. See `MOverzealousFan::onTrigger`
            // for the consumer.
            for (auto& [att_id, att_obj] : state_.objects) {
                if (!att_obj.isUnit() || !att_obj.isAtBattlefield()) continue;
                auto abf = att_obj.battlefieldId();
                if (!abf || *abf != e.battlefield) continue;
                if (att_obj.controller != e.attacker) continue;
                if (att_obj.combat_designation != CombatDesignation::Attacker) continue;
                obj.card_counters["__defend_attacker_id"] =
                    static_cast<int>(att_id);
                break;
            }
            fireTrigger(id, obj.controller, 0, TriggerType::WhenIDefend);
        }
        if ((is_attacker || is_defender) &&
            cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIAttackOrDefend)) {
            fireTrigger(id, obj.controller, 0, TriggerType::WhenIAttackOrDefend);
        }

        // Check attached gear for combat triggers
        if (is_attacker) {
            fireEquippedTriggers(id, obj.controller, TriggerType::WhenIAttack);
            fireEquippedTriggers(id, obj.controller, TriggerType::WhenIAttackOrDefend);
        }
        if (is_defender) {
            fireEquippedTriggers(id, obj.controller, TriggerType::WhenIDefend);
            fireEquippedTriggers(id, obj.controller, TriggerType::WhenIAttackOrDefend);
        }
    }

    // "When a friendly unit attacks" on the attacker's other cards (subject =
    // the attacking unit), and "When one or more friendly units attack here"
    // on the attacker's non-unit objects at this battlefield (once per combat).
    {
        std::vector<GameObjectId> attackers;
        for (auto& [id, obj] : state_.objects)
            if (obj.isUnit() && obj.controller == e.attacker &&
                obj.combat_designation == CombatDesignation::Attacker &&
                obj.battlefieldId() == e.battlefield)
                attackers.push_back(id);
        if (!attackers.empty()) {
            std::vector<std::pair<GameObjectId, GameObjectId>> fires;   // (watcher, subject)
            std::vector<GameObjectId> here_watchers;
            for (auto& [id, obj] : state_.objects) {
                if (obj.controller != e.attacker || obj.card_def_id == kInvalidId) continue;
                if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
                if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenAFriendlyUnitAttacks))
                    for (auto a : attackers) fires.push_back({id, a});
                if (!obj.isUnit() && obj.battlefieldId() == e.battlefield &&
                    cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouAttackHere))
                    here_watchers.push_back(id);
            }
            for (auto& [w, a] : fires)
                fireTrigger(w, e.attacker, 0, TriggerType::WhenAFriendlyUnitAttacks, a);
            for (auto w : here_watchers)
                fireTrigger(w, e.attacker, 0, TriggerType::WhenYouAttackHere);
        }
    }

    // "When a friendly unit attacks or defends alone" — fires on the side's
    // controller's on-board cards (incl. free-standing gear) when EXACTLY ONE
    // friendly unit participates on that side at this BF; the lone unit is the
    // subject. (Mask of Foresight 060.)
    std::vector<GameObjectId> side_attackers, side_defenders;
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.isAtBattlefield()) continue;
        auto ubf = obj.battlefieldId();
        if (!ubf || *ubf != e.battlefield) continue;
        if (obj.combat_designation == CombatDesignation::Attacker)
            side_attackers.push_back(id);
        else if (obj.combat_designation == CombatDesignation::Defender)
            side_defenders.push_back(id);
    }
    auto fireAlone = [&](GameObjectId lone, PlayerId side) {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != side || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenAUnitAttacksOrDefendsAlone))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, side, 0,
                        TriggerType::WhenAUnitAttacksOrDefendsAlone, /*subject=*/lone);
    };
    if (side_attackers.size() == 1) fireAlone(side_attackers[0], e.attacker);
    if (side_defenders.size() == 1) fireAlone(side_defenders[0], e.defender);

    // "When you defend at a battlefield" — fires on the defending player's
    // on-board cards anywhere (the unit it moves is, by definition, NOT yet at
    // the BF). The defended battlefield id is stashed on each watcher's
    // counters so the card can read it at resume time. Gated on an actual
    // defense (>=1 defending unit). (Loyal Pup 447.)
    if (!side_defenders.empty()) {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != e.defender || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenYouDefendAtABattlefield)) {
                obj.card_counters["__defend_bf"] = static_cast<int>(e.battlefield);
                watchers.push_back(id);
            }
        }
        for (auto wid : watchers)
            fireTrigger(wid, e.defender, 0,
                        TriggerType::WhenYouDefendAtABattlefield);
    }
}

void TriggerManager::onCombatEnded(const CombatEndedEvent& e) {
    // "When a combat that I was in ends," — fires win, lose or draw. APPROX:
    // combat designations are already cleared here, so every unit still at
    // the battlefield counts as having been in the combat.
    {
        std::vector<GameObjectId> present;
        for (auto& [id, obj] : state_.objects) {
            if (!obj.isUnit() || obj.card_def_id == kInvalidId) continue;
            auto unit_bf = obj.battlefieldId();
            if (!unit_bf || *unit_bf != e.battlefield) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenACombatIWasInEnds))
                present.push_back(id);
        }
        for (auto uid : present)
            fireTrigger(uid, state_.getObject(uid).controller, 0,
                        TriggerType::WhenACombatIWasInEnds);
    }

    // No fire on indecisive combat (both sides emptied, or no recall outcome).
    if (!e.winner.has_value() || *e.winner == PlayerId::None) return;
    PlayerId winner = *e.winner;

    // Legend-zone sweep: legends like Glorious Executioner / Voidreaver use
    // "When you win a combat," text encoded as WhenIWinCombat. Their legend
    // object isn't on the BF, so we need to fire them explicitly here.
    fireLegendTrigger(TriggerType::WhenIWinCombat, winner);

    // Snapshot the surviving winner-side units at this BF. We snapshot first
    // because fireTrigger / fireEquippedTriggers mutate the chain and could
    // re-enter the object map during iteration.
    std::vector<GameObjectId> winners;
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit()) continue;
        if (obj.controller != winner) continue;
        auto unit_bf = obj.battlefieldId();
        if (!unit_bf || *unit_bf != e.battlefield) continue;
        winners.push_back(id);
    }

    for (auto uid : winners) {
        if (!state_.objectExists(uid)) continue;
        const auto& obj = state_.getObject(uid);
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIWinCombat)) {
            fireTrigger(uid, obj.controller, 0, TriggerType::WhenIWinCombat);
        }
        // Equipped gear with WhenIWinCombat
        fireEquippedTriggers(uid, obj.controller, TriggerType::WhenIWinCombat);
        // Object-scoped delayed abilities watching "when IT wins a combat"
        // (e.g. Grim Resolve's "gain 2 XP when the buffed unit wins").
        checkDelayedAbilities(TriggerType::WhenIWinCombat, obj.controller, uid);
    }

    // "When you win a combat here," — the winner's non-units at that battlefield
    // (Blossoming Dreamroot, a [Deploy] gear).
    std::vector<GameObjectId> here;
    for (auto& [id, obj] : state_.objects) {
        if (obj.isUnit() || obj.controller != winner || obj.card_def_id == kInvalidId) continue;
        auto at = obj.battlefieldId();
        if (!at || *at != e.battlefield) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouWinACombatHere))
            here.push_back(id);
    }
    for (auto id : here)
        if (state_.objectExists(id)) fireTrigger(id, winner, 0, TriggerType::WhenYouWinACombatHere);
}


void TriggerManager::onUnitDied(const UnitDiedEvent& e) {
    // "When a unit dies here" â€” the battlefield card where it died.
    if (const auto* bfl = std::get_if<BattlefieldLocation>(&e.was_at))
        fireBattlefieldTriggers(TriggerType::WhenAUnitDiesHere, e.controller, bfl->id, e.object);
    // "When an enemy unit here dies," — watchers at the location it died at.
    {
        std::vector<GameObjectId> watchers;
        for (auto& [id, other] : state_.objects) {
            if (id == e.object || other.card_def_id == kInvalidId) continue;
            if (other.controller == e.controller) continue;
            if (!other.location.has_value() || *other.location != e.was_at) continue;
            if (cardFiresOn(card_registry_, other.card_def_id,
                            TriggerType::WhenAnEnemyUnitDiesHere))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, state_.getObject(wid).controller, 0,
                        TriggerType::WhenAnEnemyUnitDiesHere, /*subject=*/e.object);
    }
    // Deathknell an Equipment gave the unit ("[Deathknell] — Draw 1." in the
    // gear's effect text): the gear it had as it died (last-known information).
    if (state_.objectExists(e.object) && effect_executor_) {
        const auto& dead = state_.getObject(e.object);
        const auto gear_at_death = dead.last_known.valid ? dead.last_known.attachments
                                                         : dead.attachments;
        for (auto gear_id : gear_at_death) {
            if (!state_.objectExists(gear_id)) continue;
            const auto& gear = state_.getObject(gear_id);
            Card* gear_card = card_registry_.get(gear.card_def_id);
            if (!gear_card || gear_card->equippedTriggerType() != TriggerType::WhenIDie) continue;
            events_.logTrace("EQUIP_TRIGGER: " + gear.name + " fires on the death of " + dead.name);
            CardContext ctx{state_, events_, *effect_executor_, e.controller, gear_id};
            gear_card->onEquippedTrigger(ctx, e.object, {});
        }
    }
    if (state_.objectExists(e.object)) {
        auto& obj = state_.getObject(e.object);

        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIDie)) {
            // Karthus, Eternal: "Your Deathknell effects trigger an
            // additional time." `deathknell_double_count` is bumped per
            // controlled Karthus by recalculateAuras. Each unit adds
            // (1 + count) chain items. Empty in the default case so the
            // existing single-fire path is unchanged.
            int extra = state_.player(e.controller).deathknell_double_count +
                        deathknellExtraAt(e.controller, e.was_at);
            int fires = 1 + std::max(0, extra);
            for (int i = 0; i < fires; ++i) {
                chain_.addAbility(e.object, e.controller, obj.card_def_id);
                if (!state_.chain.items.empty()) {
                    state_.chain.items.back().fired_trigger = TriggerType::WhenIDie;
                }
            }
            if (extra > 0) {
                events_.logTrace("DEATHKNELL_DOUBLE: " + obj.name + " fires " +
                                 std::to_string(fires) + " times (Karthus aura)");
            }
        }
    }

    // "When a friendly unit dies" — fire on other objects the controller owns.
    // "When an enemy unit dies" — fire on the OPPONENT's objects (Pyke,
    // Returned: gated "while I'm at a battlefield" + once/turn in the card).
    PlayerId opp = opponent(e.controller);
    for (auto& [id, other] : state_.objects) {
        if (!other.location.has_value()) continue;
        if (id == e.object) continue;
        if (other.controller == e.controller &&
            cardFiresOn(card_registry_, other.card_def_id,
                        TriggerType::WhenAFriendlyUnitDies)) {
            fireTrigger(id, e.controller, 0, TriggerType::WhenAFriendlyUnitDies,
                        /*subject=*/e.object);
        }
        if (other.controller == opp &&
            cardFiresOn(card_registry_, other.card_def_id,
                        TriggerType::WhenAnEnemyUnitDies)) {
            fireTrigger(id, opp, 0, TriggerType::WhenAnEnemyUnitDies);
        }
    }

    // "When you kill an enemy unit" (Solari Shrine). The killer is the
    // controller of the chain item resolving, else the player whose damage
    // was marked on it (combat, or lethal damage killed at cleanup).
    {
        PlayerId killer = PlayerId::None;
        if (state_.chain.resuming.has_value()) {
            killer = state_.chain.resuming->controller;
        } else if (state_.objectExists(e.object)) {
            const auto& lk = state_.getObject(e.object).last_known;
            if (lk.valid && lk.damage_by[playerIndex(opp)] > 0) killer = opp;
        }
        if (killer == opp) {
            std::vector<GameObjectId> watchers;
            for (auto& [id, other] : state_.objects)
                if (other.location.has_value() && other.controller == opp &&
                    cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenYouKillAnEnemyUnit))
                    watchers.push_back(id);
            for (auto wid : watchers)
                fireTrigger(wid, opp, 0, TriggerType::WhenYouKillAnEnemyUnit, /*subject=*/e.object);
        }
    }

    // "When you kill a unit with a spell" (Immortal Phoenix 037). Attributed to
    // the controller of the spell currently resolving on the chain. Fans to that
    // player's on-board cards AND cards in their TRASH (Phoenix triggers from
    // trash). APPROX: keyed on a spell being the resolving chain item — covers
    // direct-kill spells cleanly; deferred damage-kills resolved after the chain
    // item clears won't see it.
    if (state_.chain.resuming.has_value() && state_.chain.resuming->is_spell) {
        const PlayerId killer = state_.chain.resuming->controller;
        std::vector<GameObjectId> watchers;
        for (auto& [id, other] : state_.objects) {
            if (id == e.object || other.card_def_id == kInvalidId) continue;
            bool on_board = other.location.has_value() && other.controller == killer;
            bool in_trash = other.zone == ZoneType::Trash && other.owner == killer;
            if (!on_board && !in_trash) continue;
            if (cardFiresOn(card_registry_, other.card_def_id,
                            TriggerType::WhenYouKillAUnitWithASpell))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, killer, 0, TriggerType::WhenYouKillAUnitWithASpell);
    }

    // Delayed abilities scoped to this dying unit (Deadly Flourish:
    // "When IT dies this turn, ..."). The opposing player created the
    // delayed ability, so we check both controllers' scoped triggers.
    checkDelayedAbilities(TriggerType::WhenIDie, e.controller, e.object);
    checkDelayedAbilities(TriggerType::WhenIDie,
                           opponent(e.controller), e.object);
}

// Extra Deathknell fires from cards at `where` ("Friendly [Deathknell] effects
// here trigger an additional time." — Detonation Amplifier).
int TriggerManager::deathknellExtraAt(PlayerId controller, const LocationId& where) const {
    if (!std::holds_alternative<BattlefieldLocation>(where)) return 0;
    int extra = 0;
    for (const auto& [id, other] : state_.objects) {
        if (other.controller != controller || other.card_def_id == kInvalidId) continue;
        if (!other.location.has_value() || *other.location != where) continue;
        if (const Card* c = card_registry_.get(other.card_def_id))
            extra += c->extraDeathknellTriggersHere();
    }
    return extra;
}

// A gear dying ("[Deathknell]" / "When I die" on gear, Bomb tokens). Unit
// deaths go through onUnitDied.
void TriggerManager::onLeftBoard(const LeftBoardEvent& e) {
    // "When this leaves the board" (Runeblade Anvil), however it left.
    if (state_.objectExists(e.object) &&
        cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                    TriggerType::WhenILeaveTheBoard))
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenILeaveTheBoard);
    // "When you recycle a rune," — a rune going from the board to the rune deck.
    if (e.card_type == CardType::Rune && e.destination == ZoneType::RuneDeck) {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != e.controller || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouRecycleARune))
                watchers.push_back(id);
        }
        for (auto wid : watchers) fireTrigger(wid, e.controller, 0, TriggerType::WhenYouRecycleARune);
        fireLegendTrigger(TriggerType::WhenYouRecycleARune, e.controller);
        return;
    }
    if (e.card_type != CardType::Gear || !e.was_killed) return;

    if (!state_.objectExists(e.object)) return;
    // "When a friendly gear dies" â€” the controller's other on-board cards.
    {
        state_.getObject(e.object).card_counters["__died_at_bf"] =
            std::holds_alternative<BattlefieldLocation>(e.was_at) ? 1 : 0;

        std::vector<GameObjectId> watchers;
        for (const auto& [id, other] : state_.objects)
            if (id != e.object && other.controller == e.controller && other.location.has_value() &&
                cardFiresOn(card_registry_, other.card_def_id, TriggerType::WhenAFriendlyGearDies))
                watchers.push_back(id);
        for (auto wid : watchers)
            fireTrigger(wid, e.controller, 0, TriggerType::WhenAFriendlyGearDies, e.object);
    }
    const auto& obj = state_.getObject(e.object);

    if (!cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIDie)) return;
    const int extra = state_.player(e.controller).deathknell_double_count +
                      deathknellExtraAt(e.controller, e.was_at);
    const int fires = 1 + std::max(0, extra);
    for (int i = 0; i < fires; ++i) {
        chain_.addAbility(e.object, e.controller, obj.card_def_id);
        if (!state_.chain.items.empty())
            state_.chain.items.back().fired_trigger = TriggerType::WhenIDie;
    }
    events_.logDebug("TRIGGER: " + obj.name + " (gear) Deathknell x" + std::to_string(fires));
}

void TriggerManager::onScore(const ScoreEvent& e) {
    auto bf_loc = BattlefieldLocation{e.battlefield};
    auto units = state_.unitsAt(bf_loc, e.player);

    // Legend-zone sweep first: legends like Green Father / Gloomist /
    // Deceiver have "When you conquer or hold," text encoded as the
    // WhenIConquerOrHold trigger on a LegendCard subclass. The legend
    // lives in champion zone, so the unit loop below won't reach it.
    // The subject is the scored battlefield's card object, so "there" /
    // "if you assigned N excess damage" can tell which battlefield it was
    // (card_helpers.h: triggeringBattlefield).
    GameObjectId scored_bf_card = kInvalidId;
    for (const auto& bf : state_.battlefields)
        if (bf.id == e.battlefield) scored_bf_card = bf.card_object_id;
    // "Your conquer effects for conquering here trigger an additional time"
    // (Red Brambleback): the scorer's conquer effects fire once more per such
    // card of theirs at this battlefield.
    // Blue Sentinel does the same for hold effects.
    int times = 1;
    for (auto uid : units)
        if (state_.objectExists(uid))
            if (const Card* c = card_registry_.get(state_.getObject(uid).card_def_id); c &&
                ((e.method == ScoreMethod::Conquer && c->conquerEffectsHereTriggerAgain()) ||
                 (e.method == ScoreMethod::Hold && c->holdEffectsHereTriggerAgain())))
                ++times;
    for (int rep = 0; rep < times; ++rep) {
        if (e.method == ScoreMethod::Conquer) {
            fireLegendTrigger(TriggerType::WhenIConquer, e.player, 0, scored_bf_card);
        }
        if (e.method == ScoreMethod::Hold) {
            fireLegendTrigger(TriggerType::WhenIHold, e.player, 0, scored_bf_card);
        }
        fireLegendTrigger(TriggerType::WhenIConquerOrHold, e.player, 0, scored_bf_card);

        for (auto uid : units) {
            if (!state_.objectExists(uid)) continue;
            auto& obj = state_.getObject(uid);
            CardDefId def = obj.card_def_id;

            // Generic "this unit conquered this turn" marker. Stamped with the
            // current turn number so a stale value from a prior turn never reads
            // as a conquer (no reset needed). Cards like Blighted Battleaxe read
            // it to ask "did the equipped unit conquer this turn?". Mirrors the
            // __defend_attacker_id capture pattern.
            if (e.method == ScoreMethod::Conquer) {
                obj.card_counters["__conquered_turn"] = state_.turn.turn_number;
            }

            if (e.method == ScoreMethod::Conquer &&
                cardFiresOn(card_registry_, def, TriggerType::WhenIConquer)) {
                fireTrigger(uid, e.player, 0, TriggerType::WhenIConquer);
            }
            if (e.method == ScoreMethod::Hold &&
                cardFiresOn(card_registry_, def, TriggerType::WhenIHold)) {
                fireTrigger(uid, e.player, 0, TriggerType::WhenIHold);
            }
            if (cardFiresOn(card_registry_, def, TriggerType::WhenIConquerOrHold)) {
                fireTrigger(uid, e.player, 0, TriggerType::WhenIConquerOrHold);
            }

            // Check attached gear for score triggers
            if (e.method == ScoreMethod::Conquer) {
                fireEquippedTriggers(uid, e.player, TriggerType::WhenIConquer);
                fireEquippedTriggers(uid, e.player, TriggerType::WhenIConquerOrHold);
            }
            if (e.method == ScoreMethod::Hold) {
                fireEquippedTriggers(uid, e.player, TriggerType::WhenIHold);
                fireEquippedTriggers(uid, e.player, TriggerType::WhenIConquerOrHold);
            }

            // Skyfall of Areion (CR): "My hold effects are also conquer effects,
            // and vice versa." If an attached gear declares the cross-fire
            // property, also fire the OPPOSITE score-type triggers on this unit
            // and its equipped gear.
            bool crossfire = false;
            for (auto gid : obj.attachments) {
                if (!state_.objectExists(gid)) continue;
                Card* gc = card_registry_.get(state_.getObject(gid).card_def_id);
                if (gc && gc->crossesHoldConquerTriggers()) { crossfire = true; break; }
            }
            if (crossfire) {
                if (e.method == ScoreMethod::Conquer) {
                    // Conquer also counts as hold → fire hold effects.
                    if (cardFiresOn(card_registry_, def, TriggerType::WhenIHold))
                        fireTrigger(uid, e.player, 0, TriggerType::WhenIHold);
                    fireEquippedTriggers(uid, e.player, TriggerType::WhenIHold);
                } else if (e.method == ScoreMethod::Hold) {
                    // Hold also counts as conquer → fire conquer effects.
                    if (cardFiresOn(card_registry_, def, TriggerType::WhenIConquer))
                        fireTrigger(uid, e.player, 0, TriggerType::WhenIConquer);
                    fireEquippedTriggers(uid, e.player, TriggerType::WhenIConquer);
                }
            }
        }
    }

    // Battlefield triggers
    for (auto& bf : state_.battlefields) {
        if (bf.id != e.battlefield) continue;
        if (!state_.objectExists(bf.card_object_id)) continue;

        CardDefId bf_def = state_.getObject(bf.card_object_id).card_def_id;

        if (e.method == ScoreMethod::Conquer &&
            cardFiresOn(card_registry_, bf_def, TriggerType::WhenYouConquerHere)) {
            fireTrigger(bf.card_object_id, e.player, 0, TriggerType::WhenYouConquerHere);
        }
        if (e.method == ScoreMethod::Hold &&
            cardFiresOn(card_registry_, bf_def, TriggerType::WhenYouHoldHere)) {
            fireTrigger(bf.card_object_id, e.player, 0, TriggerType::WhenYouHoldHere);
        }
        if (cardFiresOn(card_registry_, bf_def, TriggerType::WhenYouScoreHere)) {
            fireTrigger(bf.card_object_id, e.player, 0, TriggerType::WhenYouScoreHere);
        }
    }

    // "When you conquer here" / "When you hold here" on the scorer's non-unit
    // objects at that battlefield (gear with [Deploy]).
    {
        std::vector<std::pair<GameObjectId, TriggerType>> here;
        for (auto& [id, obj] : state_.objects) {
            if (obj.isUnit() || obj.controller != e.player || obj.card_def_id == kInvalidId) continue;
            if (obj.attached_to.has_value() || obj.battlefieldId() != e.battlefield) continue;
            if (e.method == ScoreMethod::Conquer &&
                cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouConquerHere))
                here.push_back({id, TriggerType::WhenYouConquerHere});
            if (e.method == ScoreMethod::Hold &&
                cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouHoldHere))
                here.push_back({id, TriggerType::WhenYouHoldHere});
        }
        for (int rep = 0; rep < times; ++rep)
            for (auto& [id, t] : here) fireTrigger(id, e.player, 0, t);
    }

    // Delayed "when they hold" abilities of the scorer's opponent (Ashe, Focused).
    if (e.method == ScoreMethod::Hold)
        checkDelayedAbilities(TriggerType::WhenAnOpponentHolds, opponent(e.player), kInvalidId);

    // "When an opponent scores" — fires on the non-scoring player's on-board cards.
    PlayerId opp = opponent(e.player);
    std::vector<GameObjectId> opp_watchers;
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != opp || !obj.location.has_value()) continue;
        if (obj.card_def_id == kInvalidId) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id,
                        TriggerType::WhenOpponentScores)) {
            opp_watchers.push_back(id);
        }
    }
    for (auto wid : opp_watchers)
        fireTrigger(wid, opp, 0, TriggerType::WhenOpponentScores);
}

void TriggerManager::onUnitMoved(const UnitMovedEvent& e) {
    if (!state_.objectExists(e.object)) return;
    auto& obj = state_.getObject(e.object);

    // Where it moved from, for "a battlefield I moved to or from" (Akali,
    // Deadly Weapon): battlefield id, or -1 when it left a base.
    obj.card_counters["__moved_from_bf"] =
        std::holds_alternative<BattlefieldLocation>(e.from)
            ? static_cast<int>(std::get<BattlefieldLocation>(e.from).id) : -1;
    if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIMove)) {
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenIMove);
    }
    if (std::holds_alternative<BattlefieldLocation>(e.to) &&
        cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenIMoveToFB)) {
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenIMoveToFB);
    }

    // Check attached gear for move triggers
    fireEquippedTriggers(e.object, e.controller, TriggerType::WhenIMove);
    if (std::holds_alternative<BattlefieldLocation>(e.to)) {
        fireEquippedTriggers(e.object, e.controller, TriggerType::WhenIMoveToFB);
    }

    // Fan-out: other friendly objects with WhenAFriendlyUnitMovesToFB.
    // Used by cards like Miss Fortune, Captain that watch all friendly moves.
    if (std::holds_alternative<BattlefieldLocation>(e.to)) {
        std::vector<GameObjectId> watchers;
        for (auto& [id, other] : state_.objects) {
            if (id == e.object) continue;  // skip the moving unit itself
            if (other.controller != e.controller) continue;
            if (!other.location.has_value()) continue;
            if (other.card_def_id == kInvalidId) continue;  // skip tokens
            if (cardFiresOn(card_registry_, other.card_def_id,
                            TriggerType::WhenAFriendlyUnitMovesToFB)) {
                watchers.push_back(id);
            }
        }
        for (auto wid : watchers)
            fireTrigger(wid, e.controller, 0, TriggerType::WhenAFriendlyUnitMovesToFB);
    }

    // "When a unit moves from here" — battlefield-card trigger on the FROM bf;
    // the moved unit is the subject (e.g. Back-Alley Bar gives it +1 [M]).
    if (std::holds_alternative<BattlefieldLocation>(e.from)) {
        BattlefieldId from_bf = std::get<BattlefieldLocation>(e.from).id;
        for (auto& bf : state_.battlefields) {
            if (bf.id != from_bf || !state_.objectExists(bf.card_object_id)) continue;
            CardDefId bf_def = state_.getObject(bf.card_object_id).card_def_id;
            if (cardFiresOn(card_registry_, bf_def,
                            TriggerType::WhenAUnitMovesFromHere)) {
                fireTrigger(bf.card_object_id, e.controller, 0,
                            TriggerType::WhenAUnitMovesFromHere, /*subject=*/e.object);
            }
        }
    }

    // "When an opponent moves to a battlefield" — fires on the non-mover's
    // on-board cards; the moved unit is the subject (read its location for the
    // destination). The card gates (e.g. Volibear "...other than mine").
    if (std::holds_alternative<BattlefieldLocation>(e.to)) {
        PlayerId opp = opponent(e.controller);
        std::vector<GameObjectId> opp_watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != opp || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenAnOpponentMovesToBattlefield)) {
                opp_watchers.push_back(id);
            }
        }
        for (auto wid : opp_watchers)
            fireTrigger(wid, opp, 0, TriggerType::WhenAnOpponentMovesToBattlefield,
                        /*subject=*/e.object);
    }

    // "When a friendly unit moves from my location" — fires on the controller's
    // OTHER on-board cards that shared the FROM location (Stealthy Pursuer);
    // the moved unit is the subject (read its new location to follow it).
    {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (id == e.object) continue;
            if (obj.controller != e.controller || !obj.location.has_value()) continue;
            if (!(*obj.location == e.from)) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenAFriendlyUnitMovesFromMyLocation))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, e.controller, 0,
                        TriggerType::WhenAFriendlyUnitMovesFromMyLocation,
                        /*subject=*/e.object);
    }
}

void TriggerManager::onPhaseChanged(const PhaseChangedEvent& e) {
    // "At the end of your turn"
    if (e.new_phase == TurnPhase::EndingStep) {
        for (auto& [id, obj] : state_.objects) {
            if (!obj.location.has_value()) continue;
            if (obj.controller != e.turn_player) continue;

            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::AtEndOfTurn)) {
                fireTrigger(id, e.turn_player, 0, TriggerType::AtEndOfTurn);
            }
        }
        // Legend zone
        auto& ps = state_.player(e.turn_player);
        for (auto lid : ps.legends()) {
            if (!state_.objectExists(lid)) continue;
            if (cardFiresOn(card_registry_, state_.getObject(lid).card_def_id,
                            TriggerType::AtEndOfTurn))
                fireTrigger(lid, e.turn_player, 0, TriggerType::AtEndOfTurn);
        }
        // Battlefield cards with an AtEndOfTurn trigger.
        fireBattlefieldTriggers(TriggerType::AtEndOfTurn, e.turn_player);
        // Delayed abilities scheduled for end of turn (e.g. Targon's Peak's
        // "ready up to 2 runes at the end of this turn").
        checkDelayedAbilities(TriggerType::AtEndOfTurn, e.turn_player, kInvalidId);
        // "At the end of this turn" is not "your turn": one set up by the
        // other player (Hostile Takeover played as a reaction, a conquer on
        // the opponent's turn) fires too, after the turn player's.
        checkDelayedAbilities(TriggerType::AtEndOfTurn, opponent(e.turn_player), kInvalidId);
    }

    // "At the start of your Beginning Phase"
    if (e.new_phase == TurnPhase::BeginningStep) {
        for (auto& [id, obj] : state_.objects) {
            if (!obj.location.has_value()) continue;
            if (obj.controller != e.turn_player) continue;

            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::AtStartOfBeginning)) {
                fireTrigger(id, e.turn_player, 0, TriggerType::AtStartOfBeginning);
            }
        }
        // Legend zone
        auto& ps = state_.player(e.turn_player);
        for (auto lid : ps.legends()) {
            if (!state_.objectExists(lid)) continue;
            if (cardFiresOn(card_registry_, state_.getObject(lid).card_def_id,
                            TriggerType::AtStartOfBeginning))
                fireTrigger(lid, e.turn_player, 0, TriggerType::AtStartOfBeginning);
        }
        // Battlefield cards with an AtStartOfBeginning trigger (Arena's
        // Greatest, Dusk Rose Lab), attributed to the turn player.
        fireBattlefieldTriggers(TriggerType::AtStartOfBeginning, e.turn_player);
    }

    // "At the start of your Main Phase" — used by Iascylla's delayed
    // "move enemy unit here" and similar. Both static triggers on cards
    // AND delayed abilities (registered via state.delayed_abilities) fire.
    if (e.new_phase == TurnPhase::MainPhase) {
        for (auto& [id, obj] : state_.objects) {
            if (!obj.location.has_value()) continue;
            if (obj.controller != e.turn_player) continue;

            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::AtStartOfMain)) {
                fireTrigger(id, e.turn_player, 0, TriggerType::AtStartOfMain);
            }
        }
        // Legend zone
        auto& ps = state_.player(e.turn_player);
        for (auto lid : ps.legends()) {
            if (!state_.objectExists(lid)) continue;
            if (cardFiresOn(card_registry_, state_.getObject(lid).card_def_id,
                            TriggerType::AtStartOfMain))
                fireTrigger(lid, e.turn_player, 0, TriggerType::AtStartOfMain);
        }
        // Battlefield cards with an AtStartOfMain trigger.
        fireBattlefieldTriggers(TriggerType::AtStartOfMain, e.turn_player);
        // Delayed abilities (Iascylla schedules one of these on hold)
        checkDelayedAbilities(TriggerType::AtStartOfMain, e.turn_player,
                              kInvalidId);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase-2 trigger events
// ═══════════════════════════════════════════════════════════════════════════════

void TriggerManager::onUnitReadied(const UnitReadiedEvent& e) {
    // "When you ready me" — fires on the readied object itself (Irelia, Fervent).
    if (!state_.objectExists(e.object)) return;
    if (cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                    TriggerType::WhenIAmReadied)) {
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenIAmReadied);
    }
    // "When I become ready" — effect readies here; Awaken-phase readies arrive
    // as ObjectStateChangedEvent "became_ready".
    if (cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                    TriggerType::WhenIBecomeReady)) {
        fireTrigger(e.object, e.controller, 0, TriggerType::WhenIBecomeReady);
    }
    // "When you ready a friendly unit" — fires on the controller's OTHER on-board
    // cards (e.g. Pirate's Haven), with the readied unit as the subject.
    const PlayerId controller = e.controller;
    std::vector<GameObjectId> others;
    for (auto& [id, obj] : state_.objects) {
        if (id == e.object) continue;
        if (obj.controller != controller || !obj.location.has_value()) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id,
                        TriggerType::WhenYouReadyAFriendlyUnit)) {
            others.push_back(id);
        }
    }
    for (GameObjectId id : others) {
        fireTrigger(id, controller, 0, TriggerType::WhenYouReadyAFriendlyUnit,
                    /*subject=*/e.object);
    }
}

void TriggerManager::onCardsDrawn(const CardsDrawnEvent& e) {
    // Fire WhenYouDrawACard on the drawing player's on-board watchers. The card
    // decides "is this my 2nd draw this turn" via PlayerState::draws_this_turn.
    std::vector<GameObjectId> watchers;
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != e.player || !obj.location.has_value()) continue;
        if (obj.card_def_id == kInvalidId) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouDrawACard))
            watchers.push_back(id);
    }
    for (auto wid : watchers)
        fireTrigger(wid, e.player, 0, TriggerType::WhenYouDrawACard);
}

void TriggerManager::onObjectStateChanged(const ObjectStateChangedEvent& e) {
    if (!state_.objectExists(e.object)) return;
    const PlayerId controller = state_.getObject(e.object).controller;

    // "When you use an activated ability of a gear" — fire on the gear
    // controller's on-board cards (Prize of Progress).
    if (e.what_changed == "gear_ability_used") {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != controller || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenYouActivateAGearAbility))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, 0, TriggerType::WhenYouActivateAGearAbility);
        return;
    }

    // "When a unit you control becomes [Mighty]" — fires on the controller's
    // on-board cards AND the controller's legend; the unit that crossed the
    // 5+ [M] threshold is the subject. (Fiora, Worthy 500 / Grand Duelist 519.)
    if (e.what_changed == "became_mighty") {
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != controller || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenAUnitBecomesMighty))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, 0, TriggerType::WhenAUnitBecomesMighty,
                        /*subject=*/e.object);
        // Legend zone (Grand Duelist is a legend, off-board).
        for (auto legend_id : state_.player(controller).legends())
            if (state_.objectExists(legend_id) &&
                cardFiresOn(card_registry_, state_.getObject(legend_id).card_def_id,
                            TriggerType::WhenAUnitBecomesMighty))
                fireTrigger(legend_id, controller, 0,
                            TriggerType::WhenAUnitBecomesMighty, /*subject=*/e.object);
        return;
    }

    // "When you attach an Equipment to me" — fires on the equipped unit itself
    // (attachGearToUnit / attachFree emit "equipped"). (Aphelios, Exalted 372.)
    if (e.what_changed == "chosen_by_controller") {
        // "When you choose a friendly unit" — a spell or ability its own
        // controller played chose this unit (at play or at resolution).
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (id == e.object || obj.controller != controller) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenYouChooseAFriendlyUnit))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, 0, TriggerType::WhenYouChooseAFriendlyUnit,
                        /*subject=*/e.object);
        // Battlefield cards: "when a player chooses a friendly unit here with
        // a spell". The chooser is the resolving item, else the item just
        // put on the chain.
        const bool by_spell = state_.chain.resuming.has_value()
            ? state_.chain.resuming->is_spell
            : (!state_.chain.items.empty() && state_.chain.items.back().is_spell);
        if (auto bf = state_.getObject(e.object).battlefieldId(); bf && by_spell)
            fireBattlefieldTriggers(TriggerType::WhenAFriendlyUnitChosenHere, controller, *bf);
        return;
    }

    if (e.what_changed == "became_ready") {
        if (state_.getObject(e.object).card_def_id != kInvalidId &&
            cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                        TriggerType::WhenIBecomeReady))
            fireTrigger(e.object, controller, 0, TriggerType::WhenIBecomeReady);
        return;
    }

    // "When I become exhausted," — every "exhausted" event is a ready object
    // becoming exhausted (EffectExecutor::exhaustObject only emits it then).
    if (e.what_changed == "exhausted") {
        const auto& xobj = state_.getObject(e.object);
        if (xobj.isUnit() && xobj.location.has_value() && xobj.card_def_id != kInvalidId &&
            cardFiresOn(card_registry_, xobj.card_def_id, TriggerType::WhenIBecomeExhausted))
            fireTrigger(e.object, controller, 0, TriggerType::WhenIBecomeExhausted);
        return;
    }

    if (e.what_changed == "discarded") {
        // "When you discard me," — the card is already in its owner's trash.
        const auto& dobj = state_.getObject(e.object);
        if (dobj.card_def_id != kInvalidId &&
            cardFiresOn(card_registry_, dobj.card_def_id, TriggerType::WhenIAmDiscarded))
            fireTrigger(e.object, dobj.owner, 0, TriggerType::WhenIAmDiscarded);

        // "When you discard one or more cards," (Jinx, Rebel) on the discarding
        // player's cards. One discard of several cards announces each card, so
        // a source whose ability from this discard is still waiting to be
        // finalized does not trigger again.
        const PlayerId discarder = dobj.owner;
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != discarder || obj.card_def_id == kInvalidId) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (!cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouDiscard)) continue;
            bool pending = false;
            for (const auto& ci : state_.chain.items)
                if (ci.source == id && ci.status == ChainItemStatus::Pending &&
                    ci.fired_trigger == TriggerType::WhenYouDiscard) pending = true;
            if (!pending) watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, discarder, 0, TriggerType::WhenYouDiscard, /*subject=*/e.object);
        return;
    }

    if (e.what_changed == "ability_activated") {
        // "When you play ... an activated ability" (Curator of the Sands).
        int energy = 0;
        if (auto it = state_.getObject(e.object).card_counters.find("__ability_energy_cost");
            it != state_.getObject(e.object).card_counters.end()) energy = it->second;
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != controller) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouActivateAnAbility))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, energy, TriggerType::WhenYouActivateAnAbility,
                        /*subject=*/e.object);
        return;
    }

    if (e.what_changed == "played_not_from_hand") {
        // "When you play a card from anywhere other than your hand," (trash via
        // Flow, face down, Champion Zone).
        state_.player(controller).cards_played_not_from_hand_this_turn++;
        // Mark the card itself for "if you played this from your hand" riders:
        // the turn it was played (+1, so 0 = never), read as it resolves.
        if (state_.objectExists(e.object))
            state_.getObject(e.object).card_counters["__played_not_from_hand_turn"] =
                state_.turn.turn_number + 1;
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (id == e.object || obj.controller != controller) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenYouPlayACardNotFromHand))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, 0, TriggerType::WhenYouPlayACardNotFromHand,
                        /*subject=*/e.object);
        return;
    }

    if (e.what_changed == "empowered") {
        // "When I become [Empowered]" on the object itself, then "When you
        // empower something else" on the controller's other cards + legend.
        if (state_.getObject(e.object).card_def_id != kInvalidId &&
            cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                        TriggerType::WhenIBecomeEmpowered))
            fireTrigger(e.object, controller, 0, TriggerType::WhenIBecomeEmpowered);
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (id == e.object || obj.controller != controller) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouEmpower))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, controller, 0, TriggerType::WhenYouEmpower,
                        /*subject=*/e.object);
        return;
    }

    if (e.what_changed == "burned" || e.what_changed == "banished_owned") {
        // Fires for the card's OWNER ("When you burn ...", "When you banish a
        // card you own"); subject = the burned / banished card.
        const PlayerId owner = state_.getObject(e.object).owner;
        const TriggerType t = e.what_changed == "burned"
            ? TriggerType::WhenYouBurn : TriggerType::WhenYouBanishACard;
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != owner) continue;
            if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id, t)) watchers.push_back(id);
        }
        for (auto wid : watchers) fireTrigger(wid, owner, 0, t, /*subject=*/e.object);
        return;
    }

    if (e.what_changed == "equipped") {
        if (cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                        TriggerType::WhenEquipmentAttachedToMe))
            fireTrigger(e.object, controller, 0,
                        TriggerType::WhenEquipmentAttachedToMe);
        return;
    }

    // "When you recycle one or more cards to your Main Deck" — the recycled
    // card's OWNER is the recycling player (it landed in their deck); fire on
    // their on-board cards. (Karma, Channeler 235.)
    if (e.what_changed == "recycled_main") {
        const PlayerId owner = state_.getObject(e.object).owner;
        std::vector<GameObjectId> watchers;
        for (auto& [id, obj] : state_.objects) {
            if (obj.controller != owner || !obj.location.has_value()) continue;
            if (obj.card_def_id == kInvalidId) continue;
            if (cardFiresOn(card_registry_, obj.card_def_id,
                            TriggerType::WhenYouRecycle))
                watchers.push_back(id);
        }
        for (auto wid : watchers)
            fireTrigger(wid, owner, 0, TriggerType::WhenYouRecycle);
        return;
    }

    // Only the "buffed" state change drives the remaining triggers today.
    if (e.what_changed != "buffed") return;
    // "When you buff me" — fires on the buffed object itself (Simian Ancestor).
    if (cardFiresOn(card_registry_, state_.getObject(e.object).card_def_id,
                    TriggerType::WhenIAmBuffed)) {
        fireTrigger(e.object, controller, 0, TriggerType::WhenIAmBuffed);
    }
    // "When you buff a friendly unit" — fires on the controller's OTHER on-board
    // cards (e.g. Mistfall gear). Snapshot ids first; fireTrigger mutates the chain.
    std::vector<GameObjectId> others;
    for (auto& [id, obj] : state_.objects) {
        if (id == e.object) continue;
        if (obj.controller != controller || !obj.location.has_value()) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id,
                        TriggerType::WhenYouBuffAFriendlyUnit)) {
            others.push_back(id);
        }
    }
    for (GameObjectId id : others) {
        fireTrigger(id, controller, 0, TriggerType::WhenYouBuffAFriendlyUnit,
                    /*subject=*/e.object);
    }
}

void TriggerManager::onCardHidden(const CardHiddenEvent& e) {
    // "When you hide a card" — fires on the hiding player's on-board cards
    // (Katarina, Reckless: "ready me").
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != e.player || !obj.location.has_value()) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouHideACard)) {
            fireTrigger(id, e.player, 0, TriggerType::WhenYouHideACard);
        }
    }
    // "When you hide a card here" â€” the battlefield card where it was hidden.
    if (state_.objectExists(e.card)) {
        const auto& hidden = state_.getObject(e.card);
        if (hidden.location.has_value())
            if (const auto* bfl = std::get_if<BattlefieldLocation>(&*hidden.location))
                fireBattlefieldTriggers(TriggerType::WhenYouHideACard, e.player, bfl->id, e.card);
    }

}

void TriggerManager::onPlayedFromFacedown(const PlayedFromFacedownEvent& e) {
    // "When you play a card from face down" — fires on the controller's
    // on-board cards (Katarina, Reckless: "deal 2 to an enemy unit").
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != e.player || !obj.location.has_value()) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenYouPlayFromFacedown)) {
            fireTrigger(id, e.player, 0, TriggerType::WhenYouPlayFromFacedown);
        }
    }
    fireLegendTrigger(TriggerType::WhenYouPlayFromFacedown, e.player);
}

void TriggerManager::onUnitReturnedToHand(const UnitReturnedToHandEvent& e) {
    // "When a unit here is returned to a player's hand" — battlefield-card
    // trigger (Ripper's Bay). Attributed to the bounced unit's owner ("that
    // player may pay [1] …").
    for (auto& bf : state_.battlefields) {
        if (bf.id != e.from_battlefield) continue;
        if (!state_.objectExists(bf.card_object_id)) continue;
        if (cardFiresOn(card_registry_, state_.getObject(bf.card_object_id).card_def_id,
                        TriggerType::WhenAUnitReturnsToHandHere)) {
            fireTrigger(bf.card_object_id, e.owner, 0, TriggerType::WhenAUnitReturnsToHandHere);
        }
    }
}

void TriggerManager::onShowdownStarted(const ShowdownStartedEvent& e) {
    // "When a showdown begins here" — fires on units at the showdown's
    // battlefield (Diana, Lunari) and on the battlefield card itself.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit()) continue;
        auto bf = obj.battlefieldId();
        if (!bf || *bf != e.battlefield) continue;
        if (cardFiresOn(card_registry_, obj.card_def_id, TriggerType::WhenAShowdownBeginsHere)) {
            fireTrigger(id, obj.controller, 0, TriggerType::WhenAShowdownBeginsHere);
        }
    }
    fireBattlefieldTriggers(TriggerType::WhenAShowdownBeginsHere, PlayerId::None,
                            e.battlefield);
}

void TriggerManager::onCardRevealed(const CardRevealedEvent& e) {
    // "As you look at or reveal me from the top of your deck" — fires on the
    // revealed card itself when it was on top of the Main Deck (Nocturne).
    if (e.source_zone != ZoneType::MainDeck) return;
    if (!state_.objectExists(e.card)) return;
    if (cardFiresOn(card_registry_, state_.getObject(e.card).card_def_id,
                    TriggerType::WhenIRevealedFromTop)) {
        fireTrigger(e.card, e.owner, 0, TriggerType::WhenIRevealedFromTop);
    }
}

} // namespace riftbound
