#include "obs_encoder.h"

#include "agents/evaluator.h"
#include "core/game_object.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace riftbound::ml {

namespace {

constexpr const char* kKinds[] = {
    "main", "showdown", "chain", "damage", "mulligan", "battlefield",
    "trigger", "trigger_cost", "rune", "choice", "other",
};

int8_t sat8(long v) { return static_cast<int8_t>(std::clamp(v, -127L, 127L)); }
int16_t sat16(long v) { return static_cast<int16_t>(std::clamp(v, -32767L, 32767L)); }

uint16_t vocabId(CardDefId id) {
    return id != kInvalidId && id >= 1 && id <= static_cast<CardDefId>(kCardVocabSize)
               ? static_cast<uint16_t>(id) : 0;
}

const CardDef* findDef(const CardDB& db, CardDefId id) {
    if (id == kInvalidId) return nullptr;
    const auto it = db.all().find(id);
    return it == db.all().end() ? nullptr : &it->second;
}

/// The card's printed numbers and the object's current type and keywords.
void fillStatic(const GameObject* o, const CardDef* def, ObsEntity& e) {
    if (def) {
        e.f[ent::EnergyCost] = sat8(def->energy_cost);
        e.f[ent::PowerCost] = sat8(def->power_cost);
        e.f[ent::PrintedMight] = sat8(def->might);
        e.f[ent::GearMightBonus] = sat8(def->might_bonus);
    }
    const CardType type = o ? o->card_type : def ? def->card_type : CardType::Unit;
    const SuperType super = o ? o->super_type : def ? def->super_type : SuperType::None;
    e.f[ent::TypeFirst + static_cast<int>(type)] = 1;
    e.f[ent::SuperFirst + static_cast<int>(super)] = 1;
    if (o && o->isToken()) {
        e.f[ent::SuperFirst + static_cast<int>(super)] = 0;
        e.f[ent::SuperFirst + static_cast<int>(SuperType::Token)] = 1;
    }
    const std::vector<Domain>* domains = o ? &o->domains : def ? &def->domains : nullptr;
    if (domains)
        for (Domain d : *domains)
            if (static_cast<int>(d) < static_cast<int>(Domain::Count))
                e.f[ent::DomainFirst + static_cast<int>(d)] = 1;
    for (int k = 0; k < static_cast<int>(Keyword::Count); ++k) {
        const Keyword kw = static_cast<Keyword>(k);
        if (o ? o->hasKeyword(kw) : def && def->keywords.has(kw)) e.f[ent::KeywordFirst + k] = 1;
    }
    e.f[ent::InVocab] = e.card != 0 ? 1 : 0;
}

/// State of an object on the board (or a legend / champion-zone card).
void fillDynamic(const GameState& state, const GameObject& o, ObsEntity& e) {
    e.f[ent::Might] = sat8(o.current_might);
    e.f[ent::BaseMight] = sat8(o.base_might);
    e.f[ent::Damage] = sat8(o.damage_marked);
    e.f[ent::HealthLeft] = sat8(std::max(0, o.current_might - o.damage_marked));
    e.f[ent::Exhausted] = o.is_exhausted;
    e.f[ent::Stunned] = o.is_stunned;
    e.f[ent::Buffs] = sat8(o.buff_count);
    e.f[ent::TempMight] = sat8(o.temp_might_bonus + o.temp_buff_count);
    e.f[ent::Attachments] = sat8(static_cast<long>(o.attachments.size()));
    e.f[ent::AttachmentMight] = sat8(o.attachment_might_bonus);
    e.f[ent::IsToken] = o.isToken();
    e.f[ent::Empowered] = sat8(o.empowered);
    e.f[ent::Attached] = o.attached_to.has_value();
    e.f[ent::Attacker] = o.combat_designation == CombatDesignation::Attacker;
    e.f[ent::Defender] = o.combat_designation == CombatDesignation::Defender;
    e.f[ent::MovesThisTurn] = sat8(o.moves_this_turn);
    e.f[ent::Assault] = sat8(o.assault_value + o.temp_assault_value);
    e.f[ent::Shield] = sat8(o.shield_value + o.temp_shield_value);
    e.f[ent::Deflect] = sat8(o.deflect_value);
    e.f[ent::Untargetable] = o.untargetable_by_enemy;
    e.f[ent::DamagePrevented] = o.immune_to_damage || o.prevent_next_damage_this_turn ||
                                o.prevent_damage_remaining > 0;
    e.f[ent::ControlReverts] = o.control_reverts_eot;
    e.f[ent::CantMove] = o.cant_move_on_turn == state.turn.turn_number;
    e.f[ent::Stolen] = o.owner != o.controller;
    e.f[ent::AuraMight] = sat8(o.aura_might_bonus);
    e.f[ent::DamageDoubled] = o.damage_doubled_this_turn;
    e.f[ent::TextInactive] = o.is_rules_text_inactive;
}

int battlefieldSlot(const GameState& state, BattlefieldId id) {
    for (size_t k = 0; k < state.battlefields.size(); ++k)
        if (state.battlefields[k].id == id)
            return static_cast<int>(std::min<size_t>(k, kObsMaxBattlefields - 1));
    return -1;
}

int slotOf(const GameState& state, const std::optional<LocationId>& loc) {
    if (!loc) return -1;
    const auto* at = std::get_if<BattlefieldLocation>(&*loc);
    return at ? battlefieldSlot(state, at->id) : -1;
}

} // namespace

int obsKindIndex(const char* kind) {
    const int n = static_cast<int>(std::size(kKinds));
    for (int k = 0; k < n; ++k)
        if (std::strcmp(kKinds[k], kind) == 0) return k;
    return n - 1;
}

const std::vector<std::string>& obsKindNames() {
    static const std::vector<std::string> names(std::begin(kKinds), std::end(kKinds));
    return names;
}

void encodeObs(const GameState& state, PlayerId me, const CardDB& card_db, Obs& out) {
    out.entities.clear();
    out.bags.clear();
    std::fill(std::begin(out.g), std::end(out.g), 0.0f);

    const PlayerId opp = opponent(me);
    const PlayerState& mine = state.player(me);
    const PlayerState& theirs = state.player(opp);
    const int turn = state.turn.turn_number;

    // Objects the chain's items choose, by the item's controller.
    std::vector<GameObjectId> my_chain_targets, opp_chain_targets;
    for (const ChainItem& item : state.chain.items) {
        auto& bucket = item.controller == me ? my_chain_targets : opp_chain_targets;
        bucket.insert(bucket.end(), item.targets.begin(), item.targets.end());
        bucket.insert(bucket.end(), item.chosen_objects.begin(), item.chosen_objects.end());
    }
    const auto has = [](const std::vector<GameObjectId>& v, GameObjectId id) {
        return std::find(v.begin(), v.end(), id) != v.end();
    };

    // Sort key next to each entity: (group, place, object id, tiebreak).
    std::vector<std::tuple<int, int, GameObjectId, int>> keys;
    const auto push = [&](ObsEntity&& e, int tiebreak = 0) {
        keys.emplace_back(e.group, e.place, e.object, tiebreak);
        out.entities.push_back(std::move(e));
    };

    // ── Units and gear on the board ──
    float board_might[2] = {0, 0}, units[2] = {0, 0}, ready_units[2] = {0, 0};
    struct BfTotals { int might[2] = {0, 0}, units[2] = {0, 0}, ready[2] = {0, 0}; };
    std::vector<BfTotals> bf_totals(state.battlefields.size());
    for (const auto& [id, o] : state.objects) {
        if (!o.location || o.is_hidden) continue;
        if (o.zone != ZoneType::Base && o.zone != ZoneType::BattlefieldZone) continue;
        if (!o.isUnit() && !o.isGear()) continue;
        if (o.controller != me && o.controller != opp) continue;
        const bool is_mine = o.controller == me;
        const int side = is_mine ? 0 : 1;

        ObsEntity e;
        e.object = id;
        e.card = vocabId(o.card_def_id);
        e.group = static_cast<uint8_t>(o.isUnit() ? (is_mine ? ObsGroup::MyUnit : ObsGroup::OppUnit)
                                                  : (is_mine ? ObsGroup::MyGear : ObsGroup::OppGear));
        int slot = -1;
        if (const auto* base = std::get_if<BaseLocation>(&*o.location)) {
            e.place = base->player == me ? 1 : 2;
        } else {
            const BattlefieldId bf = std::get<BattlefieldLocation>(*o.location).id;
            slot = battlefieldSlot(state, bf);
            e.place = static_cast<uint8_t>(3 + std::max(slot, 0));
            if (o.isUnit())
                for (size_t k = 0; k < state.battlefields.size(); ++k)
                    if (state.battlefields[k].id == bf) {
                        bf_totals[k].might[side] += o.current_might;
                        bf_totals[k].units[side] += 1;
                        if (!o.is_exhausted) bf_totals[k].ready[side] += 1;
                    }
        }
        if (o.isUnit()) {
            board_might[side] += static_cast<float>(o.current_might);
            units[side] += 1;
            if (!o.is_exhausted) ready_units[side] += 1;
            e.pool = static_cast<uint8_t>(
                slot >= 0 ? kObsPoolBattlefield + 2 * slot + side
                          : (is_mine ? kObsPoolMyBase : kObsPoolOppBase));
        } else if (!o.attached_to && slot < 0) {
            e.pool = static_cast<uint8_t>(is_mine ? kObsPoolMyBase : kObsPoolOppBase);
        }
        fillDynamic(state, o, e);
        e.f[ent::TargetedByOppChain] = has(is_mine ? opp_chain_targets : my_chain_targets, id) ? 1 : 0;
        e.f[ent::TargetedByMyChain] = has(is_mine ? my_chain_targets : opp_chain_targets, id) ? 1 : 0;
        fillStatic(&o, findDef(card_db, o.card_def_id), e);
        push(std::move(e));
    }

    // ── Hand, legends, champion zone ──
    for (GameObjectId id : mine.hand) {
        if (!state.objectExists(id)) continue;
        const GameObject& o = state.getObject(id);
        ObsEntity e;
        e.object = id;
        e.card = vocabId(o.card_def_id);
        e.group = static_cast<uint8_t>(ObsGroup::MyHand);
        e.pool = kObsPoolHand;
        fillStatic(&o, findDef(card_db, o.card_def_id), e);
        push(std::move(e));
    }
    const auto single = [&](GameObjectId id, ObsGroup group) {
        if (id == kInvalidId || !state.objectExists(id)) return;
        const GameObject& o = state.getObject(id);
        ObsEntity e;
        e.object = id;
        e.card = vocabId(o.card_def_id);
        e.group = static_cast<uint8_t>(group);
        fillDynamic(state, o, e);
        fillStatic(&o, findDef(card_db, o.card_def_id), e);
        push(std::move(e));
    };
    single(mine.legend_zone, ObsGroup::MyLegend);
    single(theirs.legend_zone, ObsGroup::OppLegend);
    single(mine.champion_zone, ObsGroup::MyChampion);
    single(theirs.champion_zone, ObsGroup::OppChampion);

    // ── Battlefields, and the facedown cards at them ──
    for (size_t k = 0; k < state.battlefields.size(); ++k) {
        const BattlefieldState& bf = state.battlefields[k];
        const int slot = static_cast<int>(std::min<size_t>(k, kObsMaxBattlefields - 1));
        int facedown[2] = {0, 0};
        for (GameObjectId id : bf.facedown) {
            if (!state.objectExists(id)) continue;
            const GameObject& o = state.getObject(id);
            if (o.controller != me) { ++facedown[1]; continue; }
            ++facedown[0];
            ObsEntity e;
            e.object = id;
            e.card = vocabId(o.card_def_id);
            e.group = static_cast<uint8_t>(ObsGroup::MyFacedown);
            e.place = static_cast<uint8_t>(3 + slot);
            e.pool = kObsPoolFacedown;
            e.f[ent::Hidden] = 1;
            e.f[ent::HiddenCanReact] = o.hidden_on_turn >= 0 && o.hidden_on_turn < turn;
            fillStatic(&o, findDef(card_db, o.card_def_id), e);
            push(std::move(e));
        }

        ObsEntity e;
        e.group = static_cast<uint8_t>(ObsGroup::Battlefield);
        e.place = static_cast<uint8_t>(3 + slot);
        const GameObject* card = bf.card_object_id != kInvalidId && state.objectExists(bf.card_object_id)
                                     ? &state.getObject(bf.card_object_id) : nullptr;
        e.object = card ? bf.card_object_id : kInvalidId;
        e.card = card ? vocabId(card->card_def_id) : 0;
        fillStatic(card, card ? findDef(card_db, card->card_def_id) : nullptr, e);
        const BfTotals& t = bf_totals[k];
        e.f[ent::BfMine] = bf.controller == me;
        e.f[ent::BfTheirs] = bf.controller == opp;
        e.f[ent::BfMyMight] = sat8(t.might[0]);
        e.f[ent::BfOppMight] = sat8(t.might[1]);
        e.f[ent::BfMyUnits] = sat8(t.units[0]);
        e.f[ent::BfOppUnits] = sat8(t.units[1]);
        e.f[ent::BfContested] = bf.is_contested;
        e.f[ent::BfContestedByMe] = bf.is_contested && bf.contested_by == me;
        e.f[ent::BfContestedByOpp] = bf.is_contested && bf.contested_by == opp;
        e.f[ent::BfContributedByMe] = bf.contributed_by == me;
        e.f[ent::BfScoredByMe] = mine.battlefields_scored_this_turn.count(bf.id) > 0;
        e.f[ent::BfScoredByOpp] = theirs.battlefields_scored_this_turn.count(bf.id) > 0;
        e.f[ent::BfShowdown] = bf.showdown_in_progress || bf.showdown_staged;
        e.f[ent::BfCombat] = bf.combat_in_progress || bf.combat_staged;
        e.f[ent::BfAttackerMe] = bf.attacker == me;
        e.f[ent::BfAttackerOpp] = bf.attacker == opp;
        e.f[ent::BfMyFacedown] = sat8(facedown[0]);
        e.f[ent::BfOppFacedown] = sat8(facedown[1]);
        e.f[ent::BfIsToken] = bf.is_token;
        e.f[ent::BfConqueredByMe] = bf.conquered_on_turn == turn && bf.conquered_by_player == me;
        e.f[ent::BfConqueredByOpp] = bf.conquered_on_turn == turn && bf.conquered_by_player == opp;
        e.f[ent::BfCannotScoreYet] = bf.min_turn_to_score > mine.turns_taken;
        e.f[ent::BfBlocksMoveToBase] = bf.blocks_move_to_base;
        e.f[ent::BfBlocksUnitPlay] = bf.blocks_unit_play;
        e.f[ent::BfAcceptsAnyInbound] = bf.accepts_any_inbound;
        e.f[ent::BfMyReadyUnits] = sat8(t.ready[0]);
        e.f[ent::BfOppReadyUnits] = sat8(t.ready[1]);
        e.f[ent::BfFocusHere] = state.turn.awaiting_focus_at == bf.id;
        push(std::move(e), static_cast<int>(k));
    }

    // ── The chain, newest item first ──
    const int chain_size = static_cast<int>(state.chain.items.size());
    for (int i = 0; i < chain_size; ++i) {
        const ChainItem& item = state.chain.items[i];
        const GameObject* src = item.source != kInvalidId && state.objectExists(item.source)
                                    ? &state.getObject(item.source) : nullptr;
        const CardDefId def_id = item.card_def_id != kInvalidId ? item.card_def_id
                                 : src ? src->card_def_id : kInvalidId;
        ObsEntity e;
        e.object = item.source;
        e.card = vocabId(def_id);
        e.group = static_cast<uint8_t>(item.controller == me ? ObsGroup::MyChain : ObsGroup::OppChain);
        e.pool = kObsPoolChain;
        fillStatic(src, findDef(card_db, def_id), e);
        e.f[ent::ChainDepth] = sat8(chain_size - 1 - i);
        e.f[ent::ChainIsAbility] = item.is_ability;
        push(std::move(e), chain_size - 1 - i);
    }

    // A fixed order, then the gear's hosts by index.
    std::vector<int> order(out.entities.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return keys[a] < keys[b]; });
    std::vector<ObsEntity> sorted;
    sorted.reserve(order.size());
    for (int i : order) sorted.push_back(out.entities[i]);
    out.entities.swap(sorted);
    for (ObsEntity& e : out.entities) {
        if (e.group != static_cast<uint8_t>(ObsGroup::MyGear) &&
            e.group != static_cast<uint8_t>(ObsGroup::OppGear))
            continue;
        const GameObject& o = state.getObject(e.object);
        if (!o.attached_to) continue;
        for (size_t j = 0; j < out.entities.size(); ++j) {
            const uint8_t g = out.entities[j].group;
            if (out.entities[j].object == *o.attached_to &&
                g <= static_cast<uint8_t>(ObsGroup::OppGear)) {
                e.host = static_cast<int16_t>(j);
                break;
            }
        }
    }

    // ── Bags ──
    const auto bag = [&](const std::vector<GameObjectId>& ids, ObsBag which) {
        std::vector<uint16_t> cards;
        for (GameObjectId id : ids) {
            if (!state.objectExists(id)) continue;
            const uint16_t c = vocabId(state.getObject(id).card_def_id);
            if (c != 0) cards.push_back(c);
        }
        std::sort(cards.begin(), cards.end());
        for (uint16_t c : cards) out.bags.emplace_back(static_cast<uint8_t>(which), c);
    };
    bag(mine.main_deck, ObsBag::MyDeck);
    bag(mine.trash, ObsBag::MyTrash);
    bag(theirs.trash, ObsBag::OppTrash);

    // ── Globals ──
    float* g = out.g;
    g[glob::MyScore] = static_cast<float>(mine.score);
    g[glob::OppScore] = static_cast<float>(theirs.score);
    g[glob::VictoryScore] = static_cast<float>(state.mode.victory_score);
    g[glob::MyToWin] = static_cast<float>(state.mode.victory_score - mine.score);
    g[glob::OppToWin] = static_cast<float>(state.mode.victory_score - theirs.score);
    g[glob::Turn] = static_cast<float>(turn);
    g[glob::IsMyTurn] = state.turn.turn_player == me;
    g[glob::IWentFirst] = state.turn.starting_player == me;
    g[glob::MyTurnsTaken] = static_cast<float>(mine.turns_taken);
    g[glob::OppTurnsTaken] = static_cast<float>(theirs.turns_taken);
    g[glob::PhaseMain] = state.turn.phase == TurnPhase::MainPhase;
    g[glob::Showdown] = state.turn.ns_state == NeutralShowdownState::Showdown;
    g[glob::Closed] = state.turn.oc_state == OpenClosedState::Closed;
    g[glob::ChainSize] = static_cast<float>(chain_size);
    g[glob::AwaitingMain] = state.turn.awaiting_main_action;
    g[glob::IHoldPriority] = state.turn.priority_holder == me;
    g[glob::IHoldFocus] = state.turn.focus_holder == me;

    const auto runes = [&](PlayerId p, int total, int ready, int domain_first) {
        for (GameObjectId id : state.runesInBase(p)) {
            const GameObject& r = state.getObject(id);
            g[total] += 1;
            if (r.is_exhausted) continue;
            g[ready] += 1;
            if (domain_first >= 0 && !r.domains.empty() &&
                static_cast<int>(r.domains[0]) < static_cast<int>(Domain::Count))
                g[domain_first + static_cast<int>(r.domains[0])] += 1;
        }
    };
    runes(me, glob::MyRunes, glob::MyRunesReady, glob::MyReadyDomainFirst);
    runes(opp, glob::OppRunes, glob::OppRunesReady, -1);
    g[glob::MyRuneDeck] = static_cast<float>(mine.rune_deck.size());
    g[glob::OppRuneDeck] = static_cast<float>(theirs.rune_deck.size());

    g[glob::MyPoolEnergy] = static_cast<float>(mine.rune_pool.energy);
    g[glob::MyPoolUniversal] = static_cast<float>(mine.rune_pool.universal_power);
    for (int d = 0; d < static_cast<int>(Domain::Count); ++d)
        g[glob::MyPoolDomainFirst + d] = static_cast<float>(mine.rune_pool.power[d]);
    g[glob::MyRestrictedEnergy] = static_cast<float>(mine.rune_pool.restrictedEnergyTotal());
    g[glob::MyRestrictedPower] = static_cast<float>(mine.rune_pool.restrictedPowerTotal());
    g[glob::OppPoolEnergy] = static_cast<float>(theirs.rune_pool.energy +
                                                theirs.rune_pool.restrictedEnergyTotal());
    g[glob::OppPoolPower] = static_cast<float>(theirs.rune_pool.totalPower() +
                                               theirs.rune_pool.restrictedPowerTotal());

    g[glob::MyHand] = static_cast<float>(mine.hand.size());
    g[glob::OppHand] = static_cast<float>(theirs.hand.size());
    g[glob::MyDeck] = static_cast<float>(mine.main_deck.size());
    g[glob::OppDeck] = static_cast<float>(theirs.main_deck.size());
    g[glob::MyTrash] = static_cast<float>(mine.trash.size());
    g[glob::OppTrash] = static_cast<float>(theirs.trash.size());
    g[glob::MyBanished] = static_cast<float>(mine.banishment.size());
    g[glob::OppBanished] = static_cast<float>(theirs.banishment.size());
    g[glob::MyXp] = static_cast<float>(mine.xp);
    g[glob::OppXp] = static_cast<float>(theirs.xp);
    g[glob::MyCardsPlayed] = static_cast<float>(mine.cards_played_this_turn);
    g[glob::OppCardsPlayed] = static_cast<float>(theirs.cards_played_this_turn);
    g[glob::Battlefields] = static_cast<float>(state.battlefields.size());
    for (const auto& bf : state.battlefields) {
        if (bf.controller == me) g[glob::MyHeld] += 1;
        else if (bf.controller == opp) g[glob::OppHeld] += 1;
    }
    g[glob::MyScoredThisTurn] = static_cast<float>(mine.battlefields_scored_this_turn.size());
    g[glob::OppScoredThisTurn] = static_cast<float>(theirs.battlefields_scored_this_turn.size());

    const EvalWeights defaults{};
    g[glob::HandEval] = static_cast<float>(
        std::clamp(evaluate(state, me, defaults), -2000.0, 2000.0) / 100.0);
    g[glob::MyRuneEnergy] = static_cast<float>(projectedRuneEnergy(state, me, defaults.rune_horizon));
    g[glob::OppRuneEnergy] = static_cast<float>(projectedRuneEnergy(state, opp, defaults.rune_horizon));
    g[glob::MyTrashReplayable] = static_cast<float>(trashReplayable(state, me));
    g[glob::OppTrashReplayable] = static_cast<float>(trashReplayable(state, opp));
    g[glob::MyBoardMight] = board_might[0];
    g[glob::OppBoardMight] = board_might[1];
    g[glob::MyUnits] = units[0];
    g[glob::OppUnits] = units[1];
    g[glob::MyReadyUnits] = ready_units[0];
    g[glob::OppReadyUnits] = ready_units[1];
    g[glob::MyBurnedOut] = mine.burned_out;
    g[glob::OppBurnedOut] = theirs.burned_out;
    g[glob::CantPlayCards] = mine.cant_play_cards_this_turn;
    if (state.cost_cursor && state.cost_cursor->player == me) {
        g[glob::CostEnergyLeft] = static_cast<float>(state.cost_cursor->energy_remaining);
        g[glob::CostPowerLeft] = static_cast<float>(state.cost_cursor->power_remaining);
    }
    g[glob::UnitDiedThisTurn] = state.turn.any_unit_died_this_turn;
    g[glob::MyPowerSpent] = static_cast<float>(mine.power_spent_this_turn);
}

void encodeActions(const GameState& state, const CardDB& card_db,
                   const std::vector<Intent>& legal, const Obs& obs, const char* kind,
                   int scripted_index, std::vector<ObsAction>& out) {
    out.assign(legal.size(), ObsAction{});
    thread_local SparseRow row;
    static const SparseRow kNoPosition;
    for (size_t i = 0; i < legal.size(); ++i) {
        const Intent& a = legal[i];
        ObsAction& o = out[i];

        // The action's numbers, as the gradient-boosted policy reads them.
        extractPolicyFeatures(state, card_db, legal, static_cast<int>(i), kNoPosition, kind,
                              scripted_index, row);
        for (const auto& [index, value] : row) {
            const int j = index - kPolicyActionOffset;
            if (j >= 0 && j < kObsActionDim) o.a[j] = sat16(std::lround(value));
        }

        const auto cardOf = [&](GameObjectId id) -> uint16_t {
            return id != kInvalidId && state.objectExists(id)
                       ? vocabId(state.getObject(id).card_def_id) : 0;
        };
        const GameObjectId acting = a.card != kInvalidId ? a.card : a.ability_source;
        o.acting = static_cast<int16_t>(obs.find(acting));
        o.acting_card = cardOf(acting);
        if (a.type == IntentType::ChooseBattlefield) {
            const auto& pool = state.player(a.player).battlefield_pool;
            if (a.chosen_battlefield < pool.size()) o.acting_card = vocabId(pool[a.chosen_battlefield]);
        }

        const auto chosen = [&](GameObjectId id, uint16_t card) {
            if (o.n_chosen >= kObsMaxChosen) return;
            o.chosen[o.n_chosen] = static_cast<int16_t>(obs.find(id));
            o.chosen_card[o.n_chosen] = card;
            ++o.n_chosen;
        };
        for (GameObjectId id : a.targets) chosen(id, cardOf(id));
        for (GameObjectId id : a.chosen_objects) chosen(id, cardOf(id));
        for (GameObjectId id : a.cards_to_mulligan) chosen(id, cardOf(id));
        for (const DamageAssignment& d : a.damage_assignments) chosen(d.target_unit, cardOf(d.target_unit));
        if (a.chosen_card_def != kInvalidId) chosen(kInvalidId, vocabId(a.chosen_card_def));

        for (GameObjectId id : a.units_to_move) {
            if (o.n_moved >= kObsMaxMoved) break;
            o.moved[o.n_moved++] = static_cast<int16_t>(obs.find(id));
        }
        const int move_slot = slotOf(state, a.move_destination);
        o.dest = static_cast<int16_t>(move_slot >= 0 ? move_slot : slotOf(state, a.play_location));
    }
}

std::vector<std::string> obsGlobalNames() {
    std::vector<std::string> n(kObsGlobalDim);
    for (int i = 0; i < kObsGlobalDim; ++i) n[i] = "reserved_" + std::to_string(i);
    const auto set = [&](int i, const char* name) { n[i] = name; };
    set(glob::MyScore, "my_score"); set(glob::OppScore, "opp_score");
    set(glob::VictoryScore, "victory_score"); set(glob::MyToWin, "my_to_win");
    set(glob::OppToWin, "opp_to_win"); set(glob::Turn, "turn"); set(glob::IsMyTurn, "is_my_turn");
    set(glob::IWentFirst, "i_went_first"); set(glob::MyTurnsTaken, "my_turns_taken");
    set(glob::OppTurnsTaken, "opp_turns_taken"); set(glob::PhaseMain, "phase_main");
    set(glob::Showdown, "showdown"); set(glob::Closed, "closed"); set(glob::ChainSize, "chain_size");
    set(glob::AwaitingMain, "awaiting_main"); set(glob::IHoldPriority, "i_hold_priority");
    set(glob::IHoldFocus, "i_hold_focus"); set(glob::MyRunes, "my_runes");
    set(glob::MyRunesReady, "my_runes_ready"); set(glob::MyRuneDeck, "my_rune_deck");
    set(glob::OppRunes, "opp_runes"); set(glob::OppRunesReady, "opp_runes_ready");
    set(glob::OppRuneDeck, "opp_rune_deck"); set(glob::MyPoolEnergy, "my_pool_energy");
    set(glob::MyPoolUniversal, "my_pool_universal_power");
    for (int d = 0; d < static_cast<int>(Domain::Count); ++d) {
        n[glob::MyReadyDomainFirst + d] = std::string("my_ready_runes_") + toString(static_cast<Domain>(d));
        n[glob::MyPoolDomainFirst + d] = std::string("my_pool_power_") + toString(static_cast<Domain>(d));
    }
    set(glob::MyRestrictedEnergy, "my_restricted_energy");
    set(glob::MyRestrictedPower, "my_restricted_power"); set(glob::OppPoolEnergy, "opp_pool_energy");
    set(glob::OppPoolPower, "opp_pool_power"); set(glob::MyHand, "my_hand"); set(glob::OppHand, "opp_hand");
    set(glob::MyDeck, "my_deck"); set(glob::OppDeck, "opp_deck"); set(glob::MyTrash, "my_trash");
    set(glob::OppTrash, "opp_trash"); set(glob::MyBanished, "my_banished");
    set(glob::OppBanished, "opp_banished"); set(glob::MyXp, "my_xp"); set(glob::OppXp, "opp_xp");
    set(glob::MyCardsPlayed, "my_cards_played"); set(glob::OppCardsPlayed, "opp_cards_played");
    set(glob::Battlefields, "battlefields"); set(glob::MyHeld, "my_held"); set(glob::OppHeld, "opp_held");
    set(glob::MyScoredThisTurn, "my_scored_this_turn");
    set(glob::OppScoredThisTurn, "opp_scored_this_turn"); set(glob::HandEval, "hand_eval");
    set(glob::MyRuneEnergy, "my_rune_energy"); set(glob::OppRuneEnergy, "opp_rune_energy");
    set(glob::MyTrashReplayable, "my_trash_replayable");
    set(glob::OppTrashReplayable, "opp_trash_replayable"); set(glob::MyBoardMight, "my_board_might");
    set(glob::OppBoardMight, "opp_board_might"); set(glob::MyUnits, "my_units");
    set(glob::OppUnits, "opp_units"); set(glob::MyReadyUnits, "my_ready_units");
    set(glob::OppReadyUnits, "opp_ready_units"); set(glob::MyBurnedOut, "my_burned_out");
    set(glob::OppBurnedOut, "opp_burned_out"); set(glob::CantPlayCards, "cant_play_cards");
    set(glob::CostEnergyLeft, "cost_energy_left"); set(glob::CostPowerLeft, "cost_power_left");
    set(glob::UnitDiedThisTurn, "unit_died_this_turn"); set(glob::MyPowerSpent, "my_power_spent");
    return n;
}

std::vector<std::string> obsEntityNames() {
    std::vector<std::string> n(kObsEntityDim);
    for (int i = 0; i < kObsEntityDim; ++i) n[i] = "reserved_" + std::to_string(i);
    static constexpr const char* kState[] = {
        "might", "base_might", "damage", "health_left", "exhausted", "stunned", "buffs",
        "temp_might", "attachments", "attachment_might", "is_token", "empowered", "attached",
        "attacker", "defender", "moves_this_turn", "assault", "shield", "deflect", "untargetable",
        "damage_prevented", "control_reverts", "cant_move", "hidden", "stolen", "aura_might",
        "damage_doubled", "hidden_can_react", "text_inactive", "targeted_by_opp_chain",
        "targeted_by_my_chain", "chain_depth",
    };
    for (size_t i = 0; i < std::size(kState); ++i) n[i] = kState[i];
    n[ent::EnergyCost] = "energy_cost"; n[ent::PowerCost] = "power_cost";
    n[ent::PrintedMight] = "printed_might"; n[ent::GearMightBonus] = "gear_might_bonus";
    for (int t = 0; t < 6; ++t)
        n[ent::TypeFirst + t] = std::string("type_") + toString(static_cast<CardType>(t));
    for (int s = 0; s < 4; ++s)
        n[ent::SuperFirst + s] = std::string("super_") + toString(static_cast<SuperType>(s));
    for (int d = 0; d < static_cast<int>(Domain::Count); ++d)
        n[ent::DomainFirst + d] = std::string("domain_") + toString(static_cast<Domain>(d));
    for (int k = 0; k < static_cast<int>(Keyword::Count); ++k)
        n[ent::KeywordFirst + k] = std::string("kw_") + toString(static_cast<Keyword>(k));
    n[ent::InVocab] = "in_vocab"; n[ent::ChainIsAbility] = "chain_is_ability";
    static constexpr const char* kBf[] = {
        "bf_mine", "bf_theirs", "bf_my_might", "bf_opp_might", "bf_my_units", "bf_opp_units",
        "bf_contested", "bf_contested_by_me", "bf_contested_by_opp", "bf_contributed_by_me",
        "bf_scored_by_me", "bf_scored_by_opp", "bf_showdown", "bf_combat", "bf_attacker_me",
        "bf_attacker_opp", "bf_my_facedown", "bf_opp_facedown", "bf_is_token",
        "bf_conquered_by_me", "bf_conquered_by_opp", "bf_cannot_score_yet",
        "bf_blocks_move_to_base", "bf_blocks_unit_play", "bf_accepts_any_inbound",
        "bf_my_ready_units", "bf_opp_ready_units", "bf_focus_here",
    };
    for (size_t i = 0; i < std::size(kBf); ++i) n[ent::BfMine + i] = kBf[i];
    return n;
}

} // namespace riftbound::ml
