#include <cstdio>
#include <cstdlib>
#include "game_engine.h"
#include "cards/card.h"
#include "openspiel/action_vocab.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace riftbound {

// ═══════════════════════════════════════════════════════════════════════════════
// Aura recalculation (Phase 5b — tagged effects)
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::recalculateAuras() {
    // Step 1: Clear all aura effects from all objects
    for (auto& [id, obj] : state_.objects) {
        obj.aura_effects.clear();
        obj.aura_might_bonus = 0;
        obj.aura_keywords.reset();
        obj.aura_bonus_damage_taken = 0;
        obj.immune_to_damage = false;  // re-asserted below by applyPassiveAura
        obj.spells_targeting_me_cost_reduction = 0;  // Irelia, Graceful
        obj.granted_abilities = obj.temp_granted_abilities;  // Forge / Gardens / Heimerdinger re-add theirs; "this turn" grants persist
        obj.prevent_enemy_spell_ability_damage = false;      // Esteemed Hierophant
        obj.cant_be_moved_by_enemy = false;                  // Jagged Cutlass
        obj.empower_cost_reduction = 0;              // Risen Altar
        obj.is_unit_gear = false;                    // Lady of Clockwork re-asserts hers
        // Empowered is a board status (CR 442.2): it doesn't survive leaving.
        if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) {
            obj.empowered = 0;
            obj.empower_flip_eot = 0;
        }
    }
    state_.turn.channel_phase_cap = 0;                  // Sandstone Chimera
    state_.turn.early_score_draws_instead = false;      // Otterpus
    state_.turn.all_units_enter_ready = false;          // Firebrand Raider
    state_.turn.no_awaken_unit_ready = false;
    // A legend added to the Legend Zone "while I'm on the board" (Neeko)
    // leaves when its anchor does.
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        auto& xs = state_.player(pid).extra_legends;
        for (auto it = xs.begin(); it != xs.end();) {
            const bool keep = state_.objectExists(*it) && [&] {
                const auto a = state_.getObject(*it).legend_anchor;
                return a != kInvalidId && state_.objectExists(a) &&
                       state_.getObject(a).location.has_value();
            }();
            if (keep) { ++it; continue; }
            if (state_.objectExists(*it)) {
                auto& l = state_.getObject(*it);
                l.zone = ZoneType::Banishment;
                l.location = std::nullopt;
                events_.logTrace("LEGEND: " + l.name + " leaves the Legend Zone");
            }
            it = xs.erase(it);
        }
    }

    // Step 1a: Reset per-player passive counters derived from on-board
    // presence. Bumped below by the corresponding aura scan.
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        auto& ps = state_.player(pid);
        ps.deathknell_double_count = 0;
        ps.bonus_damage_dealt = 0;   // Annie, Fiery
        ps.cannot_gain_points = false; // Tianna Crownguard
        ps.grant_friendly_units_open_bf = false; // Miss Fortune, Buccaneer
        ps.units_play_base_only = false;         // Mageseeker Warden
        ps.tokens_enter_ready = false;           // Renata Glasc, Industrialist
        ps.recall_all_on_attacker_tie = false;   // Symbol of the Solari
        ps.effects_cant_ready_my_units = false;  // Mageseeker Warden cl2
        ps.spells_have_repeat_energy = 0;        // Syndra, Transcendent
        ps.spells_have_repeat_power = 0;
        ps.repeat_cost_reduction = 0;            // Marai Spire
        ps.has_reveal_peek = false;              // Void Hatchling
        ps.zilean_present = false;               // Zilean, Time Mage
        ps.hide_for_energy = false;              // Swift Scout
        // Vendetta continuous effects.
        ps.skip_draw_phase = false;              // Endless Riches
        ps.may_play_from_trash = false;
        ps.trash_goes_to_banish = false;
        ps.flow_cost_reduction = 0;              // Stargazer
        ps.gold_adds_extra_near_victory = false; // Chem-Baroness
        ps.spell_tax_energy = 0;                 // Helm of Suppression
        ps.spell_tax_power = 0;
        ps.spell_discount_energy = 0;            // Applied Researchers
        ps.spell_discount_power = 0;
        ps.spells_cant_be_countered = false;     // Mel, Newly Awakened
        ps.extra_minus_might = 0;
        ps.locked_spell_names.clear();           // Fallen Feline
        ps.first_gear_ability_discount = 0;      // Piltovan Forge
    }
    // Per-BF aura-derived flags (Mageseeker Investigator / Noxus Saboteur /
    // Altar of Blood). Reset here; re-asserted by unit/BF applyPassiveAura.
    for (auto& bf : state_.battlefields) {
        bf.multi_move_surcharge_power[0] = bf.multi_move_surcharge_power[1] = 0;

        bf.opp_hidden_unrevealable = false;
        bf.aura_blocks_move_to_base = false;                 // Minotaur Reckoner
        bf.death_recall_for_pay = false;
        bf.ignore_tank[0] = bf.ignore_tank[1] = false;   // Dune Surfer
        bf.ignore_deflect_here = false;                  // Heisho
        bf.reaction_tax_power = 0;                       // Mystic Vortex
        bf.friendly_spell_discount_power = 0;            // Sandswept Tomb
        bf.paid_play_power = 0;                          // Dragon Roost
        bf.paid_play_tag.clear();
        bf.empower_cost_reduction[0] = bf.empower_cost_reduction[1] = 0;  // Risen Altar
        bf.extra_hide_slots = 0;                                          // Bushwhack Trap
        bf.move_tax_energy[0] = bf.move_tax_energy[1] = 0;                // Gravity Field
        bf.move_tax_power[0] = bf.move_tax_power[1] = 0;
    }

    // Step 1b: Refresh per-object targeting-protection flags from each
    // card's canBeChosenByEnemy() override. Cards on board need their
    // flag set; off-board cards stay default-false. Cheap O(n) pass over
    // GameState::objects; collocated with aura recalc so all "static
    // attributes of on-board cards" get refreshed in one place.
    // Reset first, in its own pass, so a card's aura can protect OTHER
    // objects (Alpha Wildclaw) whatever the iteration order.
    for (auto& [id, obj] : state_.objects)
        obj.untargetable_by_enemy = obj.untargetable_by_enemy_this_turn;
    for (auto& [id, obj] : state_.objects) {
        // On-board cards AND legends (LegendZone, no location) broadcast their
        // passive auras. Without including legends, legend-sourced passives
        // (Purifier's [Assault], Wuju Master's +1 [M], etc.) never fired.
        bool on_board = obj.location.has_value();
        bool legend_zone = obj.zone == ZoneType::LegendZone;
        if (!on_board && !legend_zone) continue;
        if (obj.card_def_id == kInvalidId) continue;
        const Card* card = card_registry_.get(obj.card_def_id);
        if (on_board && card && !card->canBeChosenByEnemy()) {
            obj.untargetable_by_enemy = true;
        }

        // Card-defined passive auras — each on-board card (or legend) gets a
        // chance to broadcast its passive effect (Karthus bumps the
        // controller's deathknell_double_count, future cards add their own).
        if (card) {
            card->applyPassiveAura(state_, obj.controller);
        }
    }

    // Step 1c: Battlefield-card passive auras. BF card objects have no
    // `location` (so the loop above skips them) but can carry static auras
    // (Forbidding Waste, Black Flame Altar, Ornn's Forge). Apply each,
    // attributed to the BF's controller (None if uncontrolled — the card's
    // own logic scopes by control where it matters).
    for (const auto& bf : state_.battlefields) {
        if (!state_.objectExists(bf.card_object_id)) continue;
        const auto& bobj = state_.getObject(bf.card_object_id);
        if (bobj.card_def_id == kInvalidId) continue;
        const Card* card = card_registry_.get(bobj.card_def_id);
        if (card) {
            card->applyPassiveAura(state_, bf.controller.value_or(PlayerId::None));
        }
    }

    // Step 2: Scan all objects on board for aura abilities
    for (auto& [src_id, src] : state_.objects) {
        if (!src.location.has_value() && src.zone != ZoneType::LegendZone) continue;
        if (src.card_def_id == kInvalidId) continue; // skip tokens without CardDef
        // Battlefields give their auras through Card::applyPassiveAura (the
        // loop above). A token battlefield's card object has a location, so
        // without this its text would also be read as a unit's "here" aura.
        if (src.card_type == CardType::Battlefield) continue;

        const auto& def = card_db_.get(src.card_def_id);
        auto& text = def.ability_text;
        if (text.empty()) continue;

        // Parse aura from ability_text (lowercase matching)
        std::string lower;
        lower.reserve(text.size());
        for (char c : text) lower += std::tolower(c);

        // Strip parenthetical for matching
        std::string clean;
        int depth = 0;
        for (char c : lower) {
            if (c == '(') depth++;
            else if (c == ')') { if (depth > 0) depth--; }
            else if (depth == 0) clean += c;
        }

        // Determine aura scope and effect
        struct AuraDef {
            bool is_keyword = false;
            Keyword keyword = Keyword::Count;
            int keyword_value = 0;
            int might_mod = 0;
            int might_min = 0;
            bool friendly = false;
            bool enemy = false;
            bool here_only = false;    // "here" = same battlefield as source
            bool other_only = false;   // "other" = excludes self
            bool all_friendly = false; // all friendly units anywhere
            bool tag_filter = false;
            std::string required_tag;
            bool stunned_only = false;
        };

        std::vector<AuraDef> auras;

        // "Other friendly units here have [Assault]"
        auto findAura = [&](const std::string& pattern, AuraDef base) -> bool {
            // The pattern must start its clause: "units here have" is not the
            // tail of "stunned enemy units here have" (Leona, Zealot) or of
            // "other friendly units here have".
            auto startsClause = [&](size_t p) {
                while (p > 0 && clean[p - 1] == ' ') --p;
                return p == 0 || !std::isalpha(static_cast<unsigned char>(clean[p - 1]));
            };
            auto pos = clean.find(pattern);
            while (pos != std::string::npos && !startsClause(pos)) pos = clean.find(pattern, pos + 1);
            if (pos == std::string::npos) return false;
            auto clause = clean.substr(pos, 80);

            // Determine keyword or might
            if (clause.find("[assault]") != std::string::npos) {
                base.is_keyword = true; base.keyword = Keyword::Assault; base.keyword_value = 1;
            } else if (clause.find("[shield]") != std::string::npos) {
                base.is_keyword = true; base.keyword = Keyword::Shield; base.keyword_value = 1;
            } else if (clause.find("[ganking]") != std::string::npos) {
                base.is_keyword = true; base.keyword = Keyword::Ganking;
            } else if (clause.find("[vision]") != std::string::npos) {
                base.is_keyword = true; base.keyword = Keyword::Vision;
            } else if (clause.find("[deflect]") != std::string::npos) {
                base.is_keyword = true; base.keyword = Keyword::Deflect; base.keyword_value = 1;
            } else if (clause.find("+") != std::string::npos && clause.find("[m]") != std::string::npos) {
                // Extract +N
                auto plus = clause.find('+');
                int val = 0;
                for (size_t i = plus + 1; i < clause.size() && std::isdigit(clause[i]); ++i)
                    val = val * 10 + (clause[i] - '0');
                if (val == 0) val = 1;
                base.might_mod = val;
            } else if (clause.find("-") != std::string::npos && clause.find("[m]") != std::string::npos) {
                auto minus = clause.find('-');
                int val = 0;
                for (size_t i = minus + 1; i < clause.size() && std::isdigit(clause[i]); ++i)
                    val = val * 10 + (clause[i] - '0');
                if (val == 0) val = 1;
                base.might_mod = -val;
                // Check for minimum
                auto min_pos = clause.find("minimum of");
                if (min_pos != std::string::npos) {
                    int m = 0;
                    for (size_t i = min_pos + 10; i < clause.size() && std::isdigit(clause[i]); ++i)
                        m = m * 10 + (clause[i] - '0');
                    if (m == 0) m = 1;
                    base.might_min = m;
                }
            } else {
                return false; // unrecognized aura effect
            }

            auras.push_back(base);
            return true;
        };

        // Match patterns (most specific first)
        AuraDef d;

        // "Other friendly units here have ..."
        d = {}; d.friendly = true; d.here_only = true; d.other_only = true;
        findAura("other friendly units here have", d);

        // "Other friendly units have ..."
        d = {}; d.friendly = true; d.all_friendly = true; d.other_only = true;
        findAura("other friendly units have", d);

        // "Your units have ..."
        d = {}; d.friendly = true; d.all_friendly = true;
        findAura("your units have", d);

        // "Units here have ..."
        d = {}; d.here_only = true;
        findAura("units here have", d);

        // "Stunned enemy units here have ..."
        d = {}; d.enemy = true; d.here_only = true; d.stunned_only = true;
        if (clean.find("stunned enemy units here have") != std::string::npos) {
            findAura("stunned enemy units here have", d);
        }

        // "Your Mechs each have [Assault]"
        if (clean.find("your mechs each have") != std::string::npos) {
            d = {}; d.friendly = true; d.all_friendly = true; d.tag_filter = true;
            d.required_tag = "Mech";
            findAura("your mechs each have", d);
        }

        // Step 3: Apply each aura to matching targets
        for (auto& aura : auras) {
            for (auto& [tgt_id, tgt] : state_.objects) {
                if (!tgt.isUnit()) continue;
                if (!tgt.location.has_value()) continue;

                // Self-exclusion
                if (aura.other_only && tgt_id == src_id) continue;

                // Ownership filter
                if (aura.friendly && tgt.controller != src.controller) continue;
                if (aura.enemy && tgt.controller == src.controller) continue;

                // Location filter
                if (aura.here_only) {
                    auto src_bf = src.battlefieldId();
                    auto tgt_bf = tgt.battlefieldId();
                    // Source must be at a battlefield, and target at the same one
                    if (!src_bf || !tgt_bf || *src_bf != *tgt_bf) {
                        // Also check if source IS the battlefield card
                        bool src_is_bf = src.isBattlefield();
                        if (src_is_bf) {
                            // Find which BF this card belongs to
                            bool match = false;
                            for (auto& bf : state_.battlefields) {
                                if (bf.card_object_id == src_id) {
                                    auto tbf = tgt.battlefieldId();
                                    if (tbf && *tbf == bf.id) match = true;
                                    break;
                                }
                            }
                            if (!match) continue;
                        } else {
                            continue;
                        }
                    }
                }

                // Tag filter
                if (aura.tag_filter) {
                    bool has_tag = false;
                    for (auto& tag : tgt.tags) {
                        if (tag == aura.required_tag) { has_tag = true; break; }
                    }
                    if (!has_tag) continue;
                }

                // Stunned filter
                if (aura.stunned_only && !tgt.is_stunned) continue;

                // Apply the aura effect
                GameObject::AuraEffect ae;
                ae.source = src_id;
                ae.might_bonus = aura.might_mod;
                ae.might_minimum = aura.might_min;
                if (aura.is_keyword) {
                    ae.keyword = aura.keyword;
                    ae.keyword_value = aura.keyword_value;
                }
                tgt.aura_effects.push_back(ae);
            }
        }
    }

    // Step 3b: Conditional self-effects ("If X, I have [Keyword]")
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        if (obj.card_def_id == kInvalidId) continue; // skip tokens
        const auto& def = card_db_.get(obj.card_def_id);
        auto& text = def.ability_text;
        if (text.empty()) continue;

        std::string clean;
        { int d = 0;
          for (char c : text) {
              if (c == '(') d++;
              else if (c == ')') { if (d > 0) d--; }
              else if (d == 0) clean += std::tolower(c);
          }
        }

        // "If you've discarded a card this turn, I have [Assault] and [Ganking]"
        if (clean.find("if you've discarded") != std::string::npos ||
            clean.find("if you\xe2\x80\x99ve discarded") != std::string::npos) {
            if (state_.player(obj.controller).has_discarded_this_turn) {
                if (clean.find("[assault]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Assault; ae.keyword_value = 1;
                    obj.aura_effects.push_back(ae);
                }
                if (clean.find("[ganking]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Ganking;
                    obj.aura_effects.push_back(ae);
                }
            }
        }

        // "While I'm buffed, I have [Ganking]"
        if (clean.find("while i'm buffed") != std::string::npos ||
            clean.find("while i\xe2\x80\x99m buffed") != std::string::npos) {
            if (obj.buff_count > 0) {
                if (clean.find("[ganking]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Ganking;
                    obj.aura_effects.push_back(ae);
                }
                // "While I'm buffed, I have an additional +1 [M]"
                if (clean.find("additional +1 [m]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id; ae.might_bonus = 1;
                    obj.aura_effects.push_back(ae);
                }
            }
        }

        // "While you have another unit here, I have +1 [M]" (Trusty Ramhound).
        // Only fires when this unit is at a location AND there's another
        // friendly unit at the same location (units at base count separately
        // from units at battlefields, per the card's "here" wording).
        if (clean.find("while you have another unit here") != std::string::npos &&
            clean.find("+1 [m]") != std::string::npos &&
            obj.location.has_value()) {
            for (const auto& [other_id, other] : state_.objects) {
                if (other_id == id) continue;
                if (!other.isUnit()) continue;
                if (other.controller != obj.controller) continue;
                if (!other.location.has_value()) continue;
                if (*other.location != *obj.location) continue;
                GameObject::AuraEffect ae;
                ae.source = id;
                ae.might_bonus = 1;
                obj.aura_effects.push_back(ae);
                break;
            }
        }

        // "My Might is increased by your points" (Draven, Showboat)
        if (clean.find("my might is increased by your points") != std::string::npos) {
            int pts = state_.player(obj.controller).score;
            if (pts > 0) {
                GameObject::AuraEffect ae; ae.source = id; ae.might_bonus = pts;
                obj.aura_effects.push_back(ae);
            }
        }

        // "While I'm [Mighty], I have [Deflect], [Ganking], and [Shield]"
        if (clean.find("while i'm [mighty]") != std::string::npos ||
            clean.find("while i\xe2\x80\x99m [mighty]") != std::string::npos) {
            // Mighty = 5+ might (check base + buffs + temp, before this aura)
            int pre_aura_might = obj.base_might + obj.buff_count +
                                 obj.temp_might_bonus + obj.attachment_might_bonus;
            if (pre_aura_might >= 5) {
                if (clean.find("[deflect]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Deflect; ae.keyword_value = 1;
                    obj.aura_effects.push_back(ae);
                }
                if (clean.find("[ganking]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Ganking;
                    obj.aura_effects.push_back(ae);
                }
                if (clean.find("[shield]") != std::string::npos) {
                    GameObject::AuraEffect ae; ae.source = id;
                    ae.keyword = Keyword::Shield; ae.keyword_value = 1;
                    obj.aura_effects.push_back(ae);
                }
            }
        }
    }

    // Step 3c: Equipment-granted keywords and stats
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        for (auto gear_id : obj.attachments) {
            if (!state_.objectExists(gear_id)) continue;
            Card* gear_card = card_registry_.get(state_.getObject(gear_id).card_def_id);
            if (!gear_card) continue;

            // Keywords from effect_text
            auto ekw = gear_card->equippedKeywords();
            if (ekw.bits != 0) {
                for (int k = 0; k < static_cast<int>(Keyword::Count); ++k) {
                    if (ekw.has(static_cast<Keyword>(k))) {
                        GameObject::AuraEffect ae;
                        ae.source = gear_id;
                        ae.keyword = static_cast<Keyword>(k);
                        obj.aura_effects.push_back(ae);
                    }
                }
            }
            // Assault/Shield/Deflect from effect_text
            int ea = gear_card->equippedAssault();
            if (ea > 0) {
                GameObject::AuraEffect ae;
                ae.source = gear_id;
                ae.keyword = Keyword::Assault;
                ae.keyword_value = ea;
                obj.aura_effects.push_back(ae);
            }
            int es = gear_card->equippedShield();
            if (es > 0) {
                GameObject::AuraEffect ae;
                ae.source = gear_id;
                ae.keyword = Keyword::Shield;
                ae.keyword_value = es;
                obj.aura_effects.push_back(ae);
            }
        }
    }

    // Step 4: Rebuild cached aura values on all objects
    for (auto& [id, obj] : state_.objects) {
        obj.aura_might_bonus = 0;
        obj.aura_keywords.reset();
        obj.aura_no_combat_damage = false;
        obj.aura_bonus_damage_taken = 0;
        for (auto& ae : obj.aura_effects) {
            obj.aura_might_bonus += ae.might_bonus;
            if (ae.keyword != Keyword::Count) {
                obj.aura_keywords.set(ae.keyword);
            }
            if (ae.suppress_combat_damage) obj.aura_no_combat_damage = true;
            obj.aura_bonus_damage_taken += ae.bonus_damage_taken;
        }
        // "Buffs give an additional +1 [M] to friendly units this turn" (Stand United).
        if (obj.isUnit() && obj.location.has_value() && obj.buff_count > 0 &&
            obj.controller != PlayerId::None)
            obj.aura_might_bonus += obj.buff_count *
                state_.player(obj.controller).extra_buff_might_this_turn;
        // Recompute might with new aura values
        if (obj.isUnit() && obj.location.has_value()) {
            obj.recomputeMight();
        }
    }

    // Risen Altar: battlefield-scoped [Empower] discount lands on the units there.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit()) continue;
        if (auto bf = obj.battlefieldId()) {
            for (auto& b : state_.battlefields)
                if (b.id == *bf)
                    obj.empower_cost_reduction +=
                        b.empower_cost_reduction[playerIndex(obj.controller)];
        }
    }

    refreshFlowGrants();
}

void GameEngine::refreshFlowGrants() {
    // [Flow] (Vendetta): a spell in its owner's trash may be played for its
    // Flow cost, then banished. Modeled as standing trash-replay grants that
    // are rebuilt from the trash every aura pass (banish_after marks them).
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        auto& ps = state_.player(pid);
        ps.trash_replay_grants.erase(
            std::remove_if(ps.trash_replay_grants.begin(), ps.trash_replay_grants.end(),
                [](const PlayerState::TrashReplayGrant& g) { return g.banish_after; }),
            ps.trash_replay_grants.end());
        for (auto cid : ps.trash) {
            if (!state_.objectExists(cid)) continue;
            auto& obj = state_.getObject(cid);
            if (!obj.isSpell() || obj.card_def_id == kInvalidId) continue;
            const Card* c = card_registry_.get(obj.card_def_id);
            if (!c) continue;
            PlayerState::TrashReplayGrant g;
            g.card = cid;
            g.banish_after = true;
            auto flow = c->flowCost();
            if (flow.valid) {
                g.energy = flow.energy;
                g.power = flow.power;
                g.power_domain = flow.power_domain;
                g.any_domain = flow.any_domain;
            } else {
                // Flow granted for the turn equal to the card's own cost
                // (Kennen, Storm of Shuriken).
                auto it = obj.card_counters.find("__flow_granted_turn");
                if (it == obj.card_counters.end() || it->second != state_.turn.turn_number)
                    continue;
                const auto& def = card_db_.get(obj.card_def_id);
                g.energy = def.energy_cost;
                g.power = def.power_cost;
                g.power_domain = def.domains.empty() ? Domain::Fury : def.domains[0];
            }
            // Stargazer: Flow plays cost [N] less, to a minimum of [1].
            if (ps.flow_cost_reduction > 0 && g.energy > 1)
                g.energy = std::max(1, g.energy - ps.flow_cost_reduction);
            ps.trash_replay_grants.push_back(g);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Cleanup
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::revertLapsedControl() {
    // "You control it until I leave the board" (Akshan, Mischievous). Once the
    // controlling source object is no longer in play (killed / bounced /
    // banished / otherwise off-board), control returns to the recorded owner.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.control_reverts_on_source_leave) continue;
        GameObjectId src = obj.control_source_obj;
        bool source_in_play = state_.objectExists(src) &&
            (state_.getObject(src).location.has_value() ||
             state_.getObject(src).zone == ZoneType::LegendZone);
        if (source_in_play) continue;
        if (obj.controller != obj.control_revert_to) {
            events_.logTrace("CONTROL REVERTS: " + obj.name + " (id=" +
                             std::to_string(id) + ") returns to " +
                             toString(obj.control_revert_to) +
                             " (source left the board)");
            obj.controller = obj.control_revert_to;
            events_.emit(ObjectStateChangedEvent{id, "controller_changed"});
        }
        obj.control_reverts_on_source_leave = false;
        obj.control_source_obj = kInvalidId;
        obj.control_revert_to = PlayerId::None;
    }
}

void GameEngine::cleanup() {
    // CR 322 — "Cleanup is performed repeatedly until a Cleanup occurs
    // with no new change to the game state." Phase 6q+ engine-audit
    // CRITICAL #1 fix: pre-2026-05-19 the loop set `bool changed=false`
    // and never wrote `true`, so it always exited after one pass.
    // Multi-step death cascades (lethal → control change → aura
    // recompute → another death) silently truncated.
    //
    // Change-detection: snapshot a checksum of the cleanup-mutable
    // state before each pass; if the checksum is identical after, we
    // know nothing material changed and can stop. Cheaper than a deep
    // GameState compare; covers all paths the cleanup sub-routines
    // mutate (object count + damage + zone + aura might/keywords +
    // BF control + score).
    auto checksum = [&]() -> uint64_t {
        uint64_t h = 0xCBF29CE484222325ULL; // FNV-1a seed
        auto mix = [&](uint64_t x) {
            h ^= x;
            h *= 0x100000001B3ULL;
        };
        mix(state_.objects.size());
        for (auto& [id, obj] : state_.objects) {
            mix(id);
            mix(static_cast<uint64_t>(obj.zone));
            mix(static_cast<uint64_t>(obj.damage_marked));
            mix(static_cast<uint64_t>(obj.current_might));
            mix(static_cast<uint64_t>(obj.aura_might_bonus));
            mix(obj.untargetable_by_enemy ? 0xAAA : 0xBBB);
            mix(obj.is_exhausted ? 0xCCC : 0xDDD);
            mix(static_cast<uint64_t>(obj.controller));  // control reversions
        }
        for (auto& bf : state_.battlefields) {
            mix(static_cast<uint64_t>(bf.id));
            // bf.controller is std::optional<PlayerId> — encode the
            // has_value bit + the value (if any) into one mix.
            mix(bf.controller.has_value()
                ? (0x100ULL | static_cast<uint64_t>(*bf.controller))
                : 0ULL);
            mix(bf.is_contested ? 0xEEE : 0xFFF);
        }
        for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
            mix(static_cast<uint64_t>(state_.player(pid).score));
        }
        return h;
    };

    constexpr int kMaxCleanupPasses = 20;
    for (int pass = 0; pass < kMaxCleanupPasses; ++pass) {
        if (checkWinCondition()) return;
        uint64_t before = checksum();
        processLethalDamage();
        revertLapsedControl();           // "until I leave the board" (Akshan)
        updateBattlefieldControl();
        recallMisplacedObjects();        // CR 323.6
        processContestedBattlefields();  // CR 323.2, 323.7-323.10
        recalculateAuras();
        uint64_t after = checksum();
        if (before == after) break;
    }

    // ── "Becomes Mighty" edge detection (CR: a unit is Mighty at 5+ [M]) ──
    // Run once after the cleanup loop settles so the Might layers are final.
    // We remember per-unit Mighty state in card_counters["__was_mighty"] and
    // emit a generic ObjectStateChangedEvent only on a <5 -> >=5 transition;
    // TriggerManager::onObjectStateChanged dispatches WhenAUnitBecomesMighty.
    // Falling below 5 re-arms the edge so it can fire again later this/next
    // turn. (Fiora, Worthy 500 / Grand Duelist 519.) No card-specific logic
    // here — the engine only detects and announces the state change.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        bool mighty = obj.current_might >= 5;
        bool was = obj.card_counters["__was_mighty"] != 0;
        if (mighty && !was) {
            obj.card_counters["__was_mighty"] = 1;
            events_.emit(ObjectStateChangedEvent{id, "became_mighty"});
        } else if (!mighty && was) {
            obj.card_counters["__was_mighty"] = 0;
        }
    }
}

// C-1 commit 8 — cleanup is naturally yield-free (no agent queries
// inside its body — triggers fire as events that the trigger manager
// queues onto the chain for runChain to process LATER, not during
// cleanup). The split is just a documented entry point for the
// step-machine dispatch (commit 9) so cleanup looks symmetric with
// the other phase subroutines.
void GameEngine::advanceCleanup() {
    cleanup();
}

bool GameEngine::checkWinCondition() {
    if (state_.game_over) return true;
    // The points needed to win, including battlefields that change it.
    const int effective_victory = effect_executor_->effectiveVictoryScore();

    // A player wins if they have >= victory score AND more than opponent (CR 467)
    for (auto pid : {PlayerId::Player1, PlayerId::Player2}) {
        auto& ps = state_.player(pid);
        auto& opp = state_.player(opponent(pid));
        if (ps.score >= effective_victory && ps.score > opp.score) {
            state_.game_over = true;
            state_.winner = pid;
            state_.game_over_reason = toString(pid) + std::string(" reached ") +
                                       std::to_string(ps.score) + " points";
            events_.emit(GameOverEvent{pid, state_.game_over_reason});
            return true;
        }
    }
    return false;
}

void GameEngine::processLethalDamage() {
    const bool elder_p1 = anyDamageIsLethalFor(PlayerId::Player1);
    const bool elder_p2 = anyDamageIsLethalFor(PlayerId::Player2);

    std::vector<GameObjectId> to_kill;
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit() || !obj.location.has_value()) continue;
        if (obj.damage_marked <= 0) continue;

        bool lethal = obj.hasLethalDamage();

        // Elder Dragon: any amount of that player's damage kills. "Your damage"
        // is the damage you marked, so damage a unit took from its own side
        // (or from the other player) does not count.
        if (!lethal) {
            PlayerId damager = opponent(obj.controller);
            const bool has_their_damage =
                std::min(obj.damage_marked_by[playerIndex(damager)], obj.damage_marked) > 0;
            if (has_their_damage &&
                ((damager == PlayerId::Player1 && elder_p1) ||
                 (damager == PlayerId::Player2 && elder_p2))) {
                lethal = true;
                events_.logTrace("ELDER_DRAGON: " + obj.name + " killed by any-damage rule");
            }
        }

        if (lethal) to_kill.push_back(id);
    }
    state_.death_batch++;   // lethally damaged units die at the same time
    for (auto id : to_kill) {
        killUnit(id);
    }
}

void GameEngine::updateBattlefieldControl() {
    for (auto& bf : state_.battlefields) {
        if (bf.combat_in_progress || bf.showdown_in_progress) continue;

        if (bf.controller.has_value()) {
            bool has_units = bf.hasUnitsFrom(*bf.controller, state_.objects);
            if (!has_units && state_.turn.isNeutralOpen()) {
                auto old = bf.controller;
                bf.controller = std::nullopt;
                events_.emit(ControlChangedEvent{bf.id, old, std::nullopt});
            }
        }

        // Remove facedown cards that belong to a player who doesn't control this BF (CR 323.7.5)
        std::vector<GameObjectId> to_remove;
        for (auto card_id : bf.facedown) {
            if (!state_.objectExists(card_id)) continue;
            auto& card = state_.getObject(card_id);
            if (!bf.controller.has_value() || *bf.controller != card.controller) {
                to_remove.push_back(card_id);
            }
        }
        for (auto card_id : to_remove) {
            auto it = std::find(bf.facedown.begin(), bf.facedown.end(), card_id);
            if (it != bf.facedown.end()) bf.facedown.erase(it);

            auto& card = state_.getObject(card_id);
            card.is_hidden = false;
            card.hidden_at = kInvalidId;
            card.zone = ZoneType::Trash;
            card.location = std::nullopt;
            state_.player(card.owner).trash.push_back(card_id);
            events_.logDebug(std::string("HIDDEN: ") + card.name +
                             " removed from facedown (control lost)");
        }
    }
}

void GameEngine::recallMisplacedObjects() {
    // CR 323.6: "Recall all of the following Game Objects, unless an effect
    // would indicate otherwise: Unattached non-Unit Gear located at
    // Battlefields; ... Unattached Permanents and Runes in Bases other than
    // their controller's." (A recall is not a move.)
    for (auto& [id, obj] : state_.objects) {
        if (!obj.location.has_value() || obj.is_hidden) continue;   // facedown cards stay put
        if (obj.controller == PlayerId::None) continue;
        const LocationId home{BaseLocation{obj.controller}};
        bool recall = false;
        if (obj.isGear() && !obj.is_unit_gear && !obj.attached_to.has_value() &&
            obj.isAtBattlefield() &&

            !(obj.card_def_id != kInvalidId && card_db_.get(obj.card_def_id).deploy))
            recall = true;   // CR 830.1.b.3: non-unit gear with [Deploy] stays
        if ((obj.isPermanent() || obj.isRune()) && !obj.attached_to.has_value() &&
            obj.isAtBase() && *obj.location != home)
            recall = true;
        if (!recall) continue;
        events_.logTrace("RECALL (CR 323.6): " + obj.name + " (id=" + std::to_string(id) + ")");
        obj.location = home;
        obj.zone = ZoneType::Base;
        obj.combat_designation = CombatDesignation::None;
    }
}

void GameEngine::processContestedBattlefields() {
    // CR 323.2: Attacker / Defender designations track where a Combat is.
    //  .a  a Unit at the Combat's battlefield without one gains its controller's;
    //  .b  a Unit there with the opposite one (it changed controller) is corrected;
    //  .c  a Unit anywhere else loses any designation it still has.
    for (auto& [id, obj] : state_.objects) {
        if (!obj.isUnit()) continue;
        CombatDesignation want = CombatDesignation::None;
        if (auto u_bf = obj.battlefieldId()) {
            for (const auto& bf : state_.battlefields) {
                if (bf.id != *u_bf || !bf.combat_in_progress) continue;
                if (!bf.attacker.has_value() || !bf.defender.has_value()) continue;
                if (obj.controller == *bf.attacker) want = CombatDesignation::Attacker;
                else if (obj.controller == *bf.defender) want = CombatDesignation::Defender;
            }
        }
        if (obj.combat_designation != want) {
            obj.combat_designation = want;
            obj.recomputeMight();
        }
    }

    // CR 190.3 / 323.7-323.10: Contested status and what it stages.
    for (auto& bf : state_.battlefields) {
        // CR 190.4.b: nothing changes while a Showdown or Combat is ongoing.
        if (bf.showdown_in_progress || bf.combat_in_progress) continue;

        const bool has_p1 = bf.hasUnitsFrom(PlayerId::Player1, state_.objects);
        const bool has_p2 = bf.hasUnitsFrom(PlayerId::Player2, state_.objects);
        auto present = [&](PlayerId p) {
            return p == PlayerId::Player1 ? has_p1 : p == PlayerId::Player2 ? has_p2 : false;
        };

        // CR 323.10 (190.3.b.1): remove Contested when the player who applied
        // it has no Units here. Whatever was staged on its account goes too.
        if (bf.is_contested) {
            // States built without a recorded contester (older snapshots,
            // tests) fall back to "anyone present".
            const bool contester_present = bf.contested_by != PlayerId::None
                ? present(bf.contested_by) : (has_p1 || has_p2);
            if (!contester_present) {
                events_.logTrace("CONTESTED_REMOVED: BF#" + std::to_string(bf.id) +
                                 " (contester has no units here)");
                bf.is_contested = false;
                bf.contested_by = PlayerId::None;
                bf.showdown_staged = false;
                bf.combat_staged = false;
            }
        }

        // CR 190.3.a / 323.10.a: Units at an uncontested Battlefield that their
        // controller does not control apply Contested. This also covers Units
        // that "otherwise become present" by an effect (moved, played or
        // created there, or changing controller there). Turn player first.
        if (!bf.is_contested) {
            for (PlayerId p : {state_.turn.turn_player, opponent(state_.turn.turn_player)}) {
                if (p == PlayerId::None || !present(p)) continue;
                if (bf.controller.has_value() && *bf.controller == p) continue;
                bf.is_contested = true;
                bf.contested_by = p;
                events_.emit(ContestedEvent{bf.id, p});
                break;
            }
        }

        if (!bf.is_contested) {
            bf.showdown_staged = false;
            bf.combat_staged = false;
            continue;
        }

        if (has_p1 && has_p2) {
            // CR 323.8: Units of opposing players → a Combat is staged.
            if (!bf.combat_staged) {
                bf.combat_staged = true;
                bf.showdown_staged = false;  // promote showdown → combat
            }
        } else {
            // CR 323.7 / 323.9: only the contester is here → a Showdown.
            bf.combat_staged = false;
            bf.showdown_staged = true;
        }
    }
}


GameObjectId GameEngine::instantiateCard(CardDefId def_id, PlayerId owner) {
    auto obj_id = state_.createObject();
    auto& obj = state_.getObject(obj_id);
    const auto& def = card_db_.get(def_id);

    obj.card_def_id = def_id;
    obj.owner = owner;
    obj.controller = owner;
    obj.name = def.name;
    obj.card_type = def.card_type;
    obj.super_type = def.super_type;
    obj.tags = def.tags;
    obj.domains = def.domains;
    obj.base_might = def.might;
    obj.current_might = def.might;
    obj.keywords = def.keywords;
    obj.assault_value = def.assault_value;
    obj.shield_value = def.shield_value;
    obj.deflect_value = def.deflect_value;
    obj.might_bonus = def.might_bonus;
    obj.deploy = def.deploy;

    return obj_id;

}

void GameEngine::drawCards(PlayerId player, int count) {
    // One implementation of drawing and Burn Out (CR 432) for the whole engine.
    effect_executor_->drawCards(player, count);
}

void GameEngine::channelRunes(PlayerId player, int count) {
    auto& ps = state_.player(player);
    for (int i = 0; i < count; ++i) {
        if (ps.rune_deck.empty()) break;
        auto rune_id = ps.rune_deck.back();
        ps.rune_deck.pop_back();

        auto& rune = state_.getObject(rune_id);
        rune.zone = ZoneType::Base;
        rune.location = BaseLocation{player};

        events_.logTrace("  CHANNELED: " + rune.name + " (id=" +
                         std::to_string(rune_id) + ", ready)");
        events_.emit(RuneChanneledEvent{rune_id, player});
        events_.emit(EnteredBoardEvent{
            rune_id, player, CardType::Rune, BaseLocation{player}, false});
    }
}

void GameEngine::shuffleDeck(PlayerId player) {
    auto& deck = state_.player(player).main_deck;
    std::shuffle(deck.begin(), deck.end(), rng_);
}

void GameEngine::shuffleRuneDeck(PlayerId player) {
    auto& deck = state_.player(player).rune_deck;
    std::shuffle(deck.begin(), deck.end(), rng_);
}

void GameEngine::moveUnit(GameObjectId unit_id, LocationId destination) {
    auto& unit = state_.getObject(unit_id);
    // Count this move for "moved N times this turn" effects (Kayn, Unleashed).
    // moveUnit is the single chokepoint for all unit movement (BF<->base and
    // BF<->BF, whether agent- or effect-driven), so counting here is complete.
    unit.moves_this_turn++;
    unit.location = destination;
    // Update zone type based on location
    if (std::holds_alternative<BaseLocation>(destination)) {
        unit.zone = ZoneType::Base;
    } else {
        unit.zone = ZoneType::BattlefieldZone;
    }

    // CR 719.3.a — when the top-most card moves, all attached cards move
    // with it. Phase 6q+ fix: previously attached gear stayed at the
    // original location, so a unit dragging Equipment from base to a BF
    // would leave its gear behind (or vice versa). Move every attached
    // object to the same destination and update its zone in lock-step.
    for (auto attached_id : unit.attachments) {
        if (!state_.objectExists(attached_id)) continue;
        auto& a = state_.getObject(attached_id);
        a.location = destination;
        if (std::holds_alternative<BaseLocation>(destination)) {
            a.zone = ZoneType::Base;
        } else {
            a.zone = ZoneType::BattlefieldZone;
        }
    }
}

void GameEngine::killUnit(GameObjectId unit_id) {
    auto& unit = state_.getObject(unit_id);
    auto controller = unit.controller;
    // Last-known information for "When I die" (CR 383.2.c.3.b): captured now,
    // before gear is shed and the designation and statuses are cleared.
    effect_executor_->snapshotLastKnown(unit_id);
    events_.logTrace("KILL: " + unit.name + " (id=" + std::to_string(unit_id) +
                     ", " + std::to_string(unit.current_might) + "M, dmg=" +
                     std::to_string(unit.damage_marked) + ")");

    // Deferred one-shot death replacement on the dying unit itself
    // (Tactical Retreat 737: "the next time it would die this turn, heal it,
    // exhaust it, and recall it instead"). Consulted before any source-based
    // replacement; consumes the flag and aborts the death.
    if (effect_executor_->takeDeathRecallReplacement(unit_id)) {
        events_.logDebug("REPLACEMENT (self): " + unit.name +
                         " heals/exhausts/recalls instead of dying");
        detachAllGear(unit_id);                // CR 719.5 — recall sheds gear
        auto old_loc = unit.location;
        unit.damage_marked = 0;
        unit.is_exhausted = true;
        unit.combat_designation = CombatDesignation::None;
        unit.location = BaseLocation{controller};
        unit.zone = ZoneType::Base;
        events_.emit(ObjectStateChangedEvent{unit_id, "healed"});
        events_.emit(UnitMovedEvent{unit_id, controller,
            old_loc.value_or(BaseLocation{controller}),
            BaseLocation{controller}, false});
        return;  // replacement consumed — unit does NOT die
    }

    // Altar of Blood (762): "If a unit here would die during combat, its
    // controller may pay [A][A][A] to heal it, exhaust it, and recall it
    // instead." Gated on the dying unit's battlefield carrying the flag (set by
    // Altar's applyPassiveAura) AND that BF being in combat. [A][A][A] = 3 power
    // of any domain → recycle 3 ready runes. Agent decides (only offered when
    // affordable).
    if (unit.location.has_value() &&
        std::holds_alternative<BattlefieldLocation>(*unit.location)) {
        auto bfid = std::get<BattlefieldLocation>(*unit.location).id;
        auto& abf = getBattlefield(bfid);
        if (abf.death_recall_for_pay && abf.combat_in_progress) {
            LocationId base{BaseLocation{controller}};
            std::vector<GameObjectId> ready;
            for (auto& [id, obj] : state_.objects)
                if (obj.isRune() && obj.controller == controller && !obj.is_exhausted &&
                    obj.location.has_value() && *obj.location == base)
                    ready.push_back(id);
            if (static_cast<int>(ready.size()) >= 3) {
                Intent decline; decline.type = IntentType::MakeChoice;
                decline.player = controller; decline.chosen_value = 0;
                Intent accept; accept.type = IntentType::MakeChoice;
                accept.player = controller; accept.chosen_value = 1;
                std::vector<Intent> opts = {decline, accept};
                state_.decision_index++;
                auto chosen = getAgent(controller).selectAction(state_, opts);
                recordAppliedIntent(chosen);
                if (on_decision) on_decision(state_, opts, chosen);
                if (chosen.chosen_value.value_or(0) == 1) {
                    for (int i = 0; i < 3; ++i) {  // pay [A][A][A]: recycle 3 runes
                        auto& r = state_.getObject(ready[i]);
                        const auto was_at = r.location;
                        r.location = std::nullopt;
                        r.zone = ZoneType::RuneDeck;
                        state_.player(controller).rune_deck.insert(
                            state_.player(controller).rune_deck.begin(), ready[i]);
                        if (was_at) events_.emit(LeftBoardEvent{ready[i], controller, CardType::Rune,
                                                                *was_at, ZoneType::RuneDeck, false});
                    }
                    events_.logDebug("REPLACEMENT (Altar of Blood): " + unit.name +
                                     " pays [A][A][A] -> heal/exhaust/recall");
                    detachAllGear(unit_id);
                    auto old_loc = unit.location;
                    unit.damage_marked = 0;
                    unit.is_exhausted = true;
                    unit.combat_designation = CombatDesignation::None;
                    unit.location = BaseLocation{controller};
                    unit.zone = ZoneType::Base;
                    events_.emit(ObjectStateChangedEvent{unit_id, "healed"});
                    events_.emit(UnitMovedEvent{unit_id, controller,
                        old_loc.value_or(BaseLocation{controller}),
                        BaseLocation{controller}, false});
                    return;  // survived
                }
            }
        }
    }

    // Structured replacement effects (Card::hasReplacementEffect /
    // applyReplacement). Preferred over the legacy ability_text scan below:
    // a card decides whether it replaces THIS unit's death (e.g. Guardian
    // Angel only protects the unit it's attached to). If applyReplacement
    // returns true the unit does not die.
    for (auto& [id, obj] : state_.objects) {
        if (obj.controller != controller) continue;
        if (!obj.location.has_value() && obj.zone != ZoneType::LegendZone) continue;
        if (obj.card_def_id == kInvalidId) continue;
        Card* card = card_registry_.get(obj.card_def_id);
        if (!card || !card->hasReplacementEffect()) continue;
        if (id == unit_id && !card->replacementCoversSelf()) continue;

        CardContext ctx{state_, events_, *effect_executor_, controller, id};
        if (card->applyReplacement(ctx, unit_id)) {
            events_.logDebug(std::string("REPLACEMENT: ") + obj.name +
                             " prevents " + unit.name + " from dying");
            return; // replacement consumed — unit does NOT die
        }
    }

    // No replacement — unit dies normally. Its gear is shed (CR 719.5) after
    // the death is announced below, so abilities that trigger on it see the
    // unit as it died.

    auto was_at = unit.location;
    int might = unit.current_might;

    // CR 183.1: tokens cease to exist when leaving the board. Route
    // them to Banishment (no trash entry) instead of trash. Matches
    // EffectExecutor::killObject — same rule, different kill path
    // (this one is combat lethal damage; killObject is for spell-
    // /ability-driven kills like Disintegrate).
    const bool is_token = unit.isToken();
    unit.zone = is_token ? ZoneType::Banishment : ZoneType::Trash;
    unit.last_location = unit.location;  // preserve for Deathknell triggers
    unit.location = std::nullopt;
    unit.damage_marked = 0;
    unit.combat_designation = CombatDesignation::None;
    if (!is_token) {
        state_.player(unit.owner).trash.push_back(unit_id);
    } else {
        events_.logTrace("  TOKEN CEASES TO EXIST (CR 183.1): " + unit.name +
                         " (id=" + std::to_string(unit_id) + ")");
    }

    state_.turn.any_unit_died_this_turn = true;
    state_.player(controller).unit_or_gear_died_this_turn = true;
    state_.player(controller).unit_died_this_turn = true;
    // Shadow Watcher: a friendly unit dying during its controller's Beginning
    // Phase (Awaken/Beginning/Scoring steps of that player's own turn).
    {
        TurnPhase ph = state_.turn.phase;
        bool in_beginning = (ph == TurnPhase::AwakenPhase ||
                             ph == TurnPhase::BeginningStep ||
                             ph == TurnPhase::ScoringStep);
        if (in_beginning && state_.turn.turn_player == controller)
            state_.player(controller).unit_died_in_beginning_this_turn = true;
    }
    events_.emit(UnitDiedEvent{unit_id, controller,
        was_at.value_or(BaseLocation{controller}), might});
    events_.emit(LeftBoardEvent{unit_id, controller, CardType::Unit,
        was_at.value_or(BaseLocation{controller}),
        is_token ? ZoneType::Banishment : ZoneType::Trash, true});
    detachAllGear(unit_id);
}

void GameEngine::healAllUnits() {
    for (auto& [id, obj] : state_.objects) {
        if (obj.isUnit() && obj.damage_marked > 0 && obj.location.has_value()) {
            obj.damage_marked = 0;
        }
    }
}

void GameEngine::emptyRunePools() {
    state_.players[0].rune_pool.clear();
    state_.players[1].rune_pool.clear();
}

void GameEngine::recordAppliedIntent(const Intent& chosen) {
    int slot = ::riftbound::openspiel::encodeAction(chosen, state_);
    if (slot >= 0) {
        state_.action_history.push_back(static_cast<int64_t>(slot));
    }
    // Negative slot means the Intent couldn't be encoded — should be rare
    // (action_vocab covers the full live legal set). Leaving the history
    // entry off is the safest fallback; the OpenSpiel state replay would
    // diverge anyway if there were a real encoding hole.
}

Intent GameEngine::queryAgent(PlayerId player) {
    auto actions = generateLegalActions();
    state_.decision_index++;
    if (player == PlayerId::Player1) state_.turn.turn_decisions_p1++;
    else state_.turn.turn_decisions_p2++;

    events_.logTrace("DECISION #" + std::to_string(state_.decision_index) +
                     " (" + toString(player) + "): " +
                     std::to_string(actions.size()) + " legal actions");

    auto chosen = getAgent(player).selectAction(state_, actions);
    recordAppliedIntent(chosen);

    {
        std::string choice_str = toString(chosen.type);
        if (chosen.card != kInvalidId && state_.objectExists(chosen.card)) {
            choice_str += " " + state_.getObject(chosen.card).name;
        }
        if (chosen.ability_source != kInvalidId && state_.objectExists(chosen.ability_source)) {
            choice_str += " (src=" + state_.getObject(chosen.ability_source).name + ")";
        }
        if (!chosen.targets.empty()) {
            choice_str += " tgt=[";
            for (size_t i = 0; i < chosen.targets.size(); ++i) {
                if (i > 0) choice_str += ",";
                if (state_.objectExists(chosen.targets[i]))
                    choice_str += state_.getObject(chosen.targets[i]).name;
                else
                    choice_str += "?" + std::to_string(chosen.targets[i]);
            }
            choice_str += "]";
        }
        if (!chosen.units_to_move.empty()) {
            choice_str += " units=[";
            for (size_t i = 0; i < chosen.units_to_move.size(); ++i) {
                if (i > 0) choice_str += ",";
                if (state_.objectExists(chosen.units_to_move[i]))
                    choice_str += state_.getObject(chosen.units_to_move[i]).name;
            }
            choice_str += "]";
        }
        // MakeChoice carries its "answer" in chosen_objects (discard pick,
        // predict recycle, rune exhaust target, etc.). Name them in the
        // trace so a reader can correlate the MakeChoice line with what
        // actually moved zones in the surrounding trace.
        if (!chosen.chosen_objects.empty()) {
            choice_str += " pick=[";
            for (size_t i = 0; i < chosen.chosen_objects.size(); ++i) {
                if (i > 0) choice_str += ",";
                auto oid = chosen.chosen_objects[i];
                if (state_.objectExists(oid))
                    choice_str += state_.getObject(oid).name +
                                  "(id=" + std::to_string(oid) + ")";
                else
                    choice_str += "?" + std::to_string(oid);
            }
            choice_str += "]";
        }
        if (chosen.chosen_battlefield != kInvalidId) {
            choice_str += " bf=" + std::to_string(chosen.chosen_battlefield);
        }
        // Player tag at the front so the line scans independently of
        // the surrounding DECISION # context.
        events_.logTrace("CHOSE: [" + std::string(toString(player)) +
                         "] " + choice_str);
    }

    if (on_decision) {
        on_decision(state_, actions, chosen);
    }
    return chosen;
}

AgentInterface& GameEngine::getAgent(PlayerId player) {
    return *agents_[playerIndex(player)];
}

PlayerId GameEngine::activePlayer() const {
    if (state_.turn.priority_holder.has_value())
        return *state_.turn.priority_holder;
    return state_.turn.turn_player;
}

BattlefieldState& GameEngine::getBattlefield(BattlefieldId id) {
    for (auto& bf : state_.battlefields) {
        if (bf.id == id) return bf;
    }
    throw std::runtime_error("Battlefield not found: " + std::to_string(id));
}

const BattlefieldState& GameEngine::getBattlefield(BattlefieldId id) const {
    for (auto& bf : state_.battlefields) {
        if (bf.id == id) return bf;
    }
    throw std::runtime_error("Battlefield not found: " + std::to_string(id));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Battlefield Replace/Swap-back (CR 438)
// ═══════════════════════════════════════════════════════════════════════════════

void GameEngine::replaceBattlefield(BattlefieldId original_bf,
                                     GameObjectId token_bf_card,
                                     PlayerId controller) {
    auto& bf = getBattlefield(original_bf);

    events_.logTrace("BF_REPLACE: BF#" + std::to_string(original_bf) +
                     " replaced by token (id=" + std::to_string(token_bf_card) + ")");

    // Original BF card goes to Banishment
    if (state_.objectExists(bf.card_object_id)) {
        auto& orig = state_.getObject(bf.card_object_id);
        auto owner = orig.owner;
        orig.zone = ZoneType::Banishment;
        orig.location = std::nullopt;
        state_.player(owner).banishment.push_back(bf.card_object_id);
    }

    // Token takes the slot
    bf.replaced_card = bf.card_object_id;
    bf.was_replaced = true;
    bf.is_token = true;
    bf.card_object_id = token_bf_card;
    bf.contributed_by = controller;

    // Set token card location
    auto& token_obj = state_.getObject(token_bf_card);
    token_obj.zone = ZoneType::BattlefieldZone;
    token_obj.location = BattlefieldLocation{original_bf};
}

void GameEngine::swapBackBattlefield(BattlefieldId token_bf) {
    auto& bf = getBattlefield(token_bf);
    if (!bf.was_replaced || !bf.replaced_card.has_value()) return;

    auto original_card = *bf.replaced_card;

    events_.logTrace("BF_SWAPBACK: BF#" + std::to_string(token_bf) +
                     " restoring original card (id=" + std::to_string(original_card) + ")");

    // Remove token card
    auto& token_obj = state_.getObject(bf.card_object_id);
    token_obj.zone = ZoneType::Trash;
    token_obj.location = std::nullopt;

    // Restore original from Banishment
    if (state_.objectExists(original_card)) {
        auto& orig = state_.getObject(original_card);
        auto owner = orig.owner;
        // Remove from banishment
        auto& ban = state_.player(owner).banishment;
        ban.erase(std::remove(ban.begin(), ban.end(), original_card), ban.end());

        orig.zone = ZoneType::BattlefieldZone;
        orig.location = BattlefieldLocation{token_bf};
    }

    bf.card_object_id = original_card;
    bf.replaced_card = std::nullopt;
    bf.was_replaced = false;
    bf.is_token = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Equip/Attach (CR 716-725)
// ═══════════════════════════════════════════════════════════════════════════════

bool GameEngine::attachGearToUnit(GameObjectId gear_id, GameObjectId unit_id) {
    if (!state_.objectExists(gear_id) || !state_.objectExists(unit_id)) return false;

    auto& gear = state_.getObject(gear_id);
    auto& unit = state_.getObject(unit_id);

    if (!gear.isGear() || !unit.isUnit()) return false;
    if (gear.attached_to.has_value()) return false; // already attached

    events_.logTrace("ATTACH: " + gear.name + " (id=" + std::to_string(gear_id) +
                     ") -> " + unit.name + " (id=" + std::to_string(unit_id) + ")");

    gear.attached_to = unit_id;
    gear.is_rules_text_inactive = true; // CR 718.2: rules text inactive while attached
    unit.attachments.push_back(gear_id);

    // Move gear to same location as unit
    gear.location = unit.location;
    gear.zone = unit.zone;

    // Apply might bonus (CR 472.3.c — Arithmetic layer)
    unit.attachment_might_bonus += gear.might_bonus;
    unit.recomputeMight();

    events_.emit(ObjectStateChangedEvent{gear_id, "attached"});
    events_.emit(ObjectStateChangedEvent{unit_id, "equipped"});

    return true;
}

void GameEngine::detachAllGear(GameObjectId unit_id) {
    if (!state_.objectExists(unit_id)) return;
    // Through the executor so each gear's onUnattached hook runs (CR 719.5:
    // detached gear stays where it is).
    const auto attached = state_.getObject(unit_id).attachments;   // copy
    for (auto gear_id : attached) effect_executor_->unattachGear(gear_id);
    state_.getObject(unit_id).attachments.clear();
}

} // namespace riftbound
