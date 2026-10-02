/*
 * mod-tomeofspec - the Tome of Specialization slot layer.
 *
 * CoA's spec list is twenty SLOTS. Spec I is the character's own build; each Tome of
 * Specialization (II..XX) grants the swap spell of one more slot, and the client's spec list is
 * driven entirely by that spell: a slot's button is enabled while its spell is known
 * (`CASpecListMixin:UpdateSpecButtons` -> `IsSpellKnown(SPEC_SWAP_SPELLS[id])`), and clicking it
 * casts that spell (`SpecializationUtil.ActivateSpecialization` -> `CastSpecialSpell`, which the
 * Extensions.dll sends as CMSG_CAST_SPELL for an effect-162 - SPELL_EFFECT_TALENT_SPEC_SELECT -
 * spell). The realm therefore already receives the player's choice, as a cast of the slot's own
 * spell: the spell names the slot, and the effect's payload is only a label that is cross-checked
 * against it. The cast takes as long as the client's own spell says it does - five seconds, and it
 * is interrupted by movement and damage - so the choice becomes a switch when that cast completes.
 *
 * What was missing is the realm's side of that cast: which slots are open, which one the character
 * is on, and what each slot remembers. That is all this module owns.
 *
 * A slot holds an archetype and a build - it is not a second talent system:
 *
 *   - a slot switch APPLIES a stored build through the core's own upload path
 *     (`ApplyAscensionTalentBuild`), so the validation, the investment gates, the switch guard, the
 *     archetype's defaults and the rows are the same ones a save goes through;
 *   - the state push is the core's own (`PushAscensionAdvancementState`), and the core tells the
 *     client which slot is active because the slot layer answers the two fields of 0x725
 *     (`SetAscensionActiveSpecSlotProvider`) - the core keeps the one writer of that packet;
 *   - the build a slot remembers is read out of the realm's authoritative state
 *     (`GetAscensionTalentState`), never out of the client;
 *   - the action bar is the realm's own button list (`Player::GetActionButton`,
 *     `addActionButton`, `SendActionButtons`), kept per slot beside the build and re-stated to
 *     `character_action` whenever the character changes slot - `m_activeSpec` and the spell
 *     spec masks it feeds are never touched.
 *
 * The one rule a slot switch does not carry is the unlearn bill: the Tome that opened the slot is
 * what paid for it, and the client's spec list has no dialog to price one. Nothing else differs.
 *
 * Storage lives in `player_settings`:
 *
 *   core.tomeofspec.slot.active   the slot the character is on
 *   core.tomeofspec.slot.<n>      one record for every slot the character has left at least once
 *
 * A record is this module's own and holds a build of any length: a mark, the archetype, the row
 * count, then one `(entry, rank, locked)` triple per row. The core's loadout block is deliberately
 * NOT the record, even though it also names "an archetype plus rows": its entry field is a fixed
 * 32-row array (`LoadoutMaxEntries`, the client's loadout window), so a build written into it is
 * silently clamped and every row past the thirty-second is lost - and the core's own investment
 * gates then unlearn further rows on the restore, because the rows those gates counted are exactly
 * the ones that went missing. That shape wiped saved builds; this one cannot. A record the earlier
 * build of this module wrote in the block shape is still read (`LoadLegacySlot`) and re-written
 * whole the next time its slot is left. The slot's action-bar arrangement is kept beside it.
 *
 * The slots are independent presets, and one is never filled in from another: a slot with no record
 * is FRESH when it is entered - no archetype, nothing spent, an empty bar - and only the player's
 * own save configures it. Leaving a slot writes down what it held; entering it puts that back, and
 * nothing from the slot being left travels with it.
 *
 * Deliberately absent: no row is validated here, no switch is priced here, no archetype default is
 * decided here, no preview is answered here, and the client's window state is not touched here.
 * Those are the talent system's and the archetype system's, and duplicating any of them would be
 * the second source of truth this module exists to avoid.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "StringFormat.h"

#include "AscensionCoATalentData.h"
#include "AscensionCoATalentState.h"
#include "AscensionSpecialization.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#define MOD_TOMEOFSPEC_LOG_TAG "[mod-tomeofspec] "

// The command table's types are qualified in Acore::ChatCommands.
using namespace Acore::ChatCommands;

namespace TomeOfSpec
{
/* ------------------------------------------------------------------ *
 * The client's own table                                              *
 * ------------------------------------------------------------------ *
 * `SPEC_SWAP_SPELLS` from the client's Constants.lua, in order: index n (1-based) is the spell
 * slot n is unlocked by, and slot 1's spell is the one the client casts to return to the base
 * slot. The server's copy of the table is also checked against the spells themselves at startup
 * (each one must be an effect-162 spell whose EffectBasePoints_1 is its zero-based slot), so a
 * client table and a server table that ever disagree are logged rather than silently trusted. */
constexpr uint32 SPEC_COUNT = 20;

uint32 const SPEC_SWAP_SPELLS[SPEC_COUNT] =
{
    979993, 979994, 979995, 979996, 979997,
    979986, 979987, 979988,
    84874, 84876, 84878, 84880, 84882, 84884, 84886, 84888, 84890, 84892, 84894, 84896,
};

/* The client's DEFAULT_SPEC_NAMES, with its one typo (Spec XVIX) kept out of the realm's lines. */
char const* const SPEC_NAMES[SPEC_COUNT] =
{
    "Spec I", "Spec II", "Spec III", "Spec IV", "Spec V",
    "Spec VI", "Spec VII", "Spec VIII", "Spec IX", "Spec X",
    "Spec XI", "Spec XII", "Spec XIII", "Spec XIV", "Spec XV",
    "Spec XVI", "Spec XVII", "Spec XVIII", "Spec XIX", "Spec XX",
};

char const* SpecName(uint32 slot)
{
    return slot >= 1 && slot <= SPEC_COUNT ? SPEC_NAMES[slot - 1] : "Spec ?";
}

/* The slot a swap spell belongs to: 1..SPEC_COUNT, or 0 for a spell that is not one of the
 * client's twenty. The cast IS the choice - the client's list casts the slot's own spell - so the
 * spell's identity is what names the slot, and the value the effect carries is only cross-checked
 * against it (`Switch`, `CheckSwapSpells`). Reading the slot out of that value instead would make
 * the realm depend on a convention that has two forms in the same file: the DBC's own
 * `EffectBasePoints_1` field is zero-based (it is what the client's table was built from), while the
 * value the engine hands an effect handler is one-based (`SpellEffectInfo::CalcValue` adds one when
 * `EffectDieSides_1` is one, and `Player::ActivateSpec` reads exactly that: "damage is 1 or 2, spec
 * is 0 or 1"). Confusing the two is a silent off-by-one slot, so neither is trusted here. */
uint32 SlotForSpell(uint32 spellId)
{
    for (uint32 slot = 1; slot <= SPEC_COUNT; ++slot)
        if (SPEC_SWAP_SPELLS[slot - 1] == spellId)
            return slot;

    return 0;
}

/* ------------------------------------------------------------------ *
 * Configuration                                                      *
 * ------------------------------------------------------------------ */
namespace Cfg
{
char const* const Enable            = "TomeOfSpec.Enable";
char const* const GrantStartingSlot = "TomeOfSpec.GrantStartingSlot";
char const* const AnnounceUnlocks   = "TomeOfSpec.AnnounceUnlocks";
char const* const LogLevel          = "TomeOfSpec.LogLevel";

bool   Flag(char const* key, bool fallback) { return sConfigMgr->GetOption<bool>(key, fallback); }
uint32 Number(char const* key, uint32 fallback) { return sConfigMgr->GetOption<uint32>(key, fallback); }

/* The whole module stands behind this one switch. With it off nothing claims a slot: the swap
 * spells keep whatever behavior they have without a script, and the core's 0x725 push keeps the
 * archetype mapping it uses when no slot layer is registered. */
bool Enabled() { return Flag(Enable, true); }
} // namespace Cfg

void Report(uint32 level, std::string const &message)
{
    if (Cfg::Number(Cfg::LogLevel, 1) < level)
        return;

    LOG_INFO("coa", MOD_TOMEOFSPEC_LOG_TAG "{}", message);
}

bool Owns(Player *player)
{
    return player && Cfg::Enabled() && IsAscensionCustomClassId(player->getClass());
}

/* ------------------------------------------------------------------ *
 * The slot space                                                      *
 * ------------------------------------------------------------------ */

/* The client's rule, and the only one: slot 1 is always known, every other slot needs its swap
 * spell (`SpecializationUtil.IsSpecializationUnlocked`). Tomes grant exactly those spells, so the
 * unlock state has one representation and it is the spellbook's. */
bool Unlocked(Player *player, uint32 slot)
{
    if (slot < 1 || slot > SPEC_COUNT)
        return false;

    if (slot == 1)
        return true;

    return player->HasSpell(SPEC_SWAP_SPELLS[slot - 1]);
}

/* Slots open sequentially, so the count stops at the first locked one - the same walk the client's
 * `GetNumSpecializationsUnlocked` does, and the number the 0x725 push carries. */
uint32 UnlockedCount(Player *player)
{
    uint32 count = 0;
    for (uint32 slot = 1; slot <= SPEC_COUNT && Unlocked(player, slot); ++slot)
        count = slot;

    return count;
}

std::string SlotSetting(uint32 slot)
{
    return "core.tomeofspec.slot." + std::to_string(slot);
}

constexpr char ACTIVE_SLOT_SETTING[] = "core.tomeofspec.slot.active";

uint32 StoredSlot(Player *player)
{
    PlayerSettingVector const *values = player->FindPlayerSettings(ACTIVE_SLOT_SETTING);
    if (!values || values->empty())
        return 1;

    uint32 const slot = (*values)[0].value;
    return slot >= 1 && slot <= SPEC_COUNT ? slot : 1;
}

void StoreSlot(Player *player, uint32 slot)
{
    player->UpdatePlayerSetting(ACTIVE_SLOT_SETTING, 0, slot);
}

/* The slot the character is on. A slot whose spell is gone is not a slot it can be on - the
 * client disables the button - so the answer falls back to the first slot. The fallback is written
 * down by the callers that can cause it (login, revoke, a switch), never by this read. */
uint32 CurrentSlot(Player *player)
{
    uint32 const stored = StoredSlot(player);
    return Unlocked(player, stored) ? stored : 1;
}

/* ------------------------------------------------------------------ *
 * Per-slot records                                                    *
 * ------------------------------------------------------------------ */

/* What a slot remembers: the archetype it was left on, and the build it held. A record with no
 * archetype is not written (there is nothing to remember yet); "Configured" is the record's
 * existence, which is what tells a slot that was left from one that was never entered. */
struct SlotState
{
    bool Configured = false;
    uint32 SpecId = 0;
    std::vector<AscensionCoATalentState::KnownEntry> Entries;
};

std::vector<uint32> SettingValues(Player *player, std::string const &setting)
{
    std::vector<uint32> values;
    if (PlayerSettingVector const *stored = player->FindPlayerSettings(setting))
    {
        values.reserve(stored->size());
        for (PlayerSetting const &entry : *stored)
            values.push_back(entry.value);
    }

    return values;
}

/* The first word of every record this module writes. A record is read by shape, and this mark is
 * what tells the shape apart from the loadout blocks the earlier build of this module wrote into
 * the same setting: such a block begins with its loadout count, a number no larger than the
 * client's window, so a word no count can be is unambiguous. */
constexpr uint32 SLOT_RECORD_MARK = 0x544F4D53;   // 'TOMS'

/* A record in the loadout-block shape the earlier build of this module wrote: the build clamped to
 * the client loadout window's 32 rows, so a longer build already lost everything past the
 * thirty-second row when it was written. It is still read - the rows it kept are the player's -
 * and it is re-written in this module's own shape the next time its slot is left. The shape is
 * recognised by its first word, the loadout count of the block, which is exactly 1 for every record
 * that build wrote. */
SlotState LoadLegacySlot(Player *player, uint32 slot, std::vector<uint32> const &values)
{
    SlotState state;
    std::vector<AscensionCoATalentState::Loadout> loadouts;
    std::string activeUuid;
    if (!AscensionCoATalentState::ParseLoadoutBlock(values.data(), values.size(),
            AscensionCoATalentState::LoadoutRecordStride(AscensionCoATalentState::TalentStateRevision),
            loadouts, activeUuid) || loadouts.empty() || loadouts.front().Uuid.empty() ||
        !loadouts.front().SpecId)
        return state;

    Report(1, Acore::StringFormat("slot {} of {} holds a capped loadout-shape record ({} row(s)); "
        "read as it is and rewritten whole when the slot is next left",
        slot, player->GetName(), loadouts.front().Entries.size()));
    state.Configured = true;
    state.SpecId = loadouts.front().SpecId;
    state.Entries = loadouts.front().Entries;
    return state;
}

SlotState LoadSlot(Player *player, uint32 slot)
{
    SlotState state;
    if (slot < 1 || slot > SPEC_COUNT)
        return state;

    std::vector<uint32> const values = SettingValues(player, SlotSetting(slot));
    if (values.empty())
        return state;

    if (values[0] == SLOT_RECORD_MARK)
    {
        /* `[mark][archetype][count][entry, rank, locked]...`, whole and uncapped: the reader takes
         * exactly the row count the record carries, and a record that cannot hold that count is
         * unreadable rather than half-read. */
        if (values.size() >= 3)
        {
            uint32 const specId = values[1];
            std::size_t const count = values[2];
            if (specId && values.size() >= 3 + count * 3)
            {
                state.Configured = true;
                state.SpecId = specId;
                state.Entries.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                    state.Entries.push_back({ values[3 + index * 3], values[4 + index * 3], 0,
                                              values[5 + index * 3] != 0, 0 });
                return state;
            }
        }

        Report(0, Acore::StringFormat("slot {} of {} holds an unreadable record and is treated as blank",
            slot, player->GetName()));
        return state;
    }

    if (values[0] == 1)
        return LoadLegacySlot(player, slot, values);

    /* A word that is neither this module's mark nor the loadout count of the old shape: not a
     * record, so not read - which is also the state `clearslots` leaves behind. */
    return state;
}

void StoreSlotRecord(Player *player, uint32 slot, uint32 specId,
                     std::vector<AscensionCoATalentState::KnownEntry> const &entries)
{
    /* The whole build, never a part of it. This record is the only place a slot's preset lives
     * while the character is on another slot, so a row dropped here is a row the player has lost -
     * and the core's own gate sweep on the restore can then unlearn further rows, because the rows
     * those gates counted are exactly the ones that went missing. */
    std::vector<uint32> record;
    record.reserve(3 + entries.size() * 3);
    record.push_back(SLOT_RECORD_MARK);
    record.push_back(specId);
    record.push_back(uint32(entries.size()));
    for (AscensionCoATalentState::KnownEntry const &entry : entries)
    {
        record.push_back(entry.EntryId);
        record.push_back(entry.Rank);
        record.push_back(entry.Locked ? 1u : 0u);
    }

    std::string const setting = SlotSetting(slot);
    std::size_t const previous = SettingValues(player, setting).size();
    for (std::size_t index = 0; index < record.size(); ++index)
        player->UpdatePlayerSetting(setting, uint32(index), record[index]);
    for (std::size_t index = record.size(); index < previous; ++index)
        player->UpdatePlayerSetting(setting, uint32(index), 0);
}

/* The record of the slot being left, read out of the realm at the moment of departure - so a save,
 * a reset or an archetype switch that happened while the slot was active is part of what the slot
 * remembers. The client is never the source: a slot that holds what the client shows would lose
 * everything the realm refused. */
void RecordSlot(Player *player, uint32 slot)
{
    uint32 const specId = GetAscensionActiveSpecialization(player);
    if (!specId)
        return;   // no archetype chosen yet: nothing authoritative to remember

    std::vector<AscensionCoATalentState::KnownEntry> const entries = GetAscensionTalentState(player);
    StoreSlotRecord(player, slot, specId, entries);
    Report(1, Acore::StringFormat("{} left: slot {} remembers archetype {} with {} row(s)",
        player->GetName(), slot, specId, entries.size()));
}

/* ------------------------------------------------------------------ *
 * Per-slot action bars                                                *
 * ------------------------------------------------------------------ *
 * A slot's arrangement is part of its preset. The live bar - the buttons the character holds, and
 * the `character_action` rows for spec 0 the client is sent - belongs to the slot that is active,
 * and only to it: leaving a slot writes its arrangement down, entering one puts its own in place,
 * and a slot that was never arranged gets an empty bar, exactly as a slot that was never configured
 * gets no archetype. Nothing is copied from the slot being left, and nothing falls back to it.
 *
 * The live bar is also re-stated to the database the moment it changes slot. The core loads spec
 * 0's rows at login - and AscensionCompat reads them again for the character's own spec - so leaving
 * the previous slot's rows behind is what would put the previous slot's arrangement back on a login
 * or a crash. The rows are written as unchanged, because what is on disk is what memory holds: the
 * core's own save then only touches the buttons the player changes afterwards.
 *
 * `Player::m_activeSpec` is deliberately not used as the slot key: it is the vanilla dual-spec
 * dimension, its mask is eight bits (`SPEC_MASK_ALL` covers specs 0..7), and every spell the
 * character knows is filtered through it - a slot index in there would hide the character's own
 * spells. The slot is this module's dimension; the bar is the core's own list of buttons. */

std::string BarSetting(uint32 slot)
{
    return "core.tomeofspec.bar." + std::to_string(slot);
}

struct SlotBar
{
    std::vector<std::pair<uint8, uint32>> Buttons;   // button -> the packed action the core stores
};

SlotBar LiveBar(Player *player)
{
    SlotBar bar;
    for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        if (ActionButton const *action = player->GetActionButton(button))
            bar.Buttons.emplace_back(button, action->packedData);

    return bar;
}

/* Empty when the slot was never arranged. The first value is the count and every button is the
 * (button, packed action) pair behind it; the packed word carries the action's type with it, so a
 * spell, a macro, an item and an equipment set all travel with their button. */
SlotBar LoadSlotBar(Player *player, uint32 slot)
{
    SlotBar bar;
    if (slot < 1 || slot > SPEC_COUNT)
        return bar;

    std::vector<uint32> const values = SettingValues(player, BarSetting(slot));
    if (values.empty())
        return bar;

    std::size_t const count = std::min<std::size_t>(values[0], (values.size() - 1) / 2);
    for (std::size_t index = 0; index < count; ++index)
    {
        uint32 const button = values[2 * index + 1];
        uint32 const packed = values[2 * index + 2];
        if (button >= MAX_ACTION_BUTTONS || !packed)
            continue;

        bar.Buttons.emplace_back(uint8(button), packed);
    }

    return bar;
}

void StoreSlotBar(Player *player, uint32 slot, SlotBar const &bar)
{
    std::string const setting = BarSetting(slot);
    std::size_t const previous = SettingValues(player, setting).size();
    player->UpdatePlayerSetting(setting, 0, uint32(bar.Buttons.size()));
    for (std::size_t index = 0; index < bar.Buttons.size(); ++index)
    {
        player->UpdatePlayerSetting(setting, uint32(2 * index + 1), bar.Buttons[index].first);
        player->UpdatePlayerSetting(setting, uint32(2 * index + 2), bar.Buttons[index].second);
    }
    for (std::size_t index = 2 * bar.Buttons.size() + 1; index < previous; ++index)
        player->UpdatePlayerSetting(setting, uint32(index), 0);
}

/* The live bar becomes `bar`'s: memory, the database and the client in one transition, so the three
 * cannot disagree about which slot the character is on. */
void PutBarOnCharacter(Player *player, SlotBar const &bar)
{
    for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        player->removeActionButton(button);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM `character_action` WHERE `guid` = {} AND `spec` = 0",
        player->GetGUID().GetRawValue());
    for (auto const &[button, packed] : bar.Buttons)
    {
        uint32 const action = ACTION_BUTTON_ACTION(packed);
        uint32 const type = uint32(ACTION_BUTTON_TYPE(packed));
        ActionButton *live = player->addActionButton(button, action, uint8(type));
        if (!live)
            continue;

        trans->Append("INSERT INTO `character_action` (`guid`, `spec`, `button`, `action`, `type`) "
            "VALUES ({}, 0, {}, {}, {})", player->GetGUID().GetRawValue(), uint32(button), action, type);
        live->uState = ACTIONBUTTON_UNCHANGED;
    }
    CharacterDatabase.CommitTransaction(trans);

    player->SendActionButtons(1);
}

void RecordSlotBar(Player *player, uint32 slot)
{
    SlotBar const bar = LiveBar(player);
    StoreSlotBar(player, slot, bar);
    Report(1, Acore::StringFormat("{} left: slot {} remembers its {} bar button(s)",
        player->GetName(), slot, bar.Buttons.size()));
}

/* ------------------------------------------------------------------ *
 * The switch                                                          *
 * ------------------------------------------------------------------ *
 * One routine, the same for every slot; what differs is only which preset is applied. A slot that
 * has never been configured is fresh: it is entered with no archetype and an empty bar, so a new
 * slot never starts as a copy of the one being left - the two are independent presets, and only the
 * player's own save ever fills one in. */
bool SwitchToSlot(Player *player, uint32 slot, std::string *refusal)
{
    if (!Owns(player))
        return false;

    if (slot < 1 || slot > SPEC_COUNT)
    {
        *refusal = Acore::StringFormat("There is no specialization slot {}.", slot);
        return false;
    }

    if (!Unlocked(player, slot))
    {
        *refusal = Acore::StringFormat("{} is not unlocked yet.", SpecName(slot));
        return false;
    }

    uint32 const current = CurrentSlot(player);
    if (slot == current)
        return true;

    RecordSlot(player, current);
    RecordSlotBar(player, current);

    /* A slot with no record has no archetype of its own yet: it is applied as the empty state a
     * character has before choosing one, and the player's next save is what configures it. */
    SlotState const target = LoadSlot(player, slot);
    if (!ApplyAscensionTalentBuild(player, target.Configured ? target.SpecId : 0, target.Entries, refusal))
        return false;

    PutBarOnCharacter(player, LoadSlotBar(player, slot));

    StoreSlot(player, slot);
    return true;
}

/* Whether the character is on the slot it is stored on, and what to do when it is not: the slot it
 * can no longer be on (its spell is gone, so the client has no button for it) gives up what it held,
 * and the slot that takes over gets its own preset - build and bar, empty if it was never
 * configured. The callers that can cause this (login, revoke) write the fallback down; no read ever
 * does. */
void NormalizeActiveSlot(Player *player)
{
    uint32 const stored = StoredSlot(player);
    uint32 const current = CurrentSlot(player);
    if (current == stored)
        return;

    RecordSlot(player, stored);
    RecordSlotBar(player, stored);

    SlotState const target = LoadSlot(player, current);
    std::string refusal;
    if (!ApplyAscensionTalentBuild(player, target.Configured ? target.SpecId : 0, target.Entries, &refusal))
        Report(0, Acore::StringFormat("{} could not take up {}: {}", player->GetName(),
            SpecName(current), refusal));

    PutBarOnCharacter(player, LoadSlotBar(player, current));
    StoreSlot(player, current);
    Report(1, Acore::StringFormat("{} fell back from slot {} to slot {}", player->GetName(),
        stored, current));
}

void AnnounceSlot(Player *player, uint32 slot)
{
    if (!player->IsInWorld())
        return;

    ChatHandler(player->GetSession()).SendSysMessage(
        Acore::StringFormat("You are now on {}.", SpecName(slot)));
}

/* ------------------------------------------------------------------ *
 * The client's 0x725 push                                             *
 * ------------------------------------------------------------------ */

/* The core owns the packet; this is what its two fields mean for a slot character. */
AscensionActiveSpecSlot AnswerActiveSpec(Player *player)
{
    AscensionActiveSpecSlot answer;
    answer.Slot = CurrentSlot(player) - 1;   // the client's field is zero-based
    answer.Count = UnlockedCount(player);
    return answer;
}

void PushSlotState(Player *player, char const *reason)
{
    PushAscensionAdvancementState(player);
    Report(1, Acore::StringFormat("pushed slot {} of {} (archetype {}) [{}]", CurrentSlot(player),
        UnlockedCount(player), GetAscensionActiveSpecialization(player), reason));
}

/* ------------------------------------------------------------------ *
 * Granting                                                           *
 * ------------------------------------------------------------------ */

bool GrantSlot(Player *player, uint32 slot)
{
    if (slot < 1 || slot > SPEC_COUNT || player->HasSpell(SPEC_SWAP_SPELLS[slot - 1]))
        return false;

    player->learnSpell(SPEC_SWAP_SPELLS[slot - 1]);
    return true;
}

bool RevokeSlot(Player *player, uint32 slot)
{
    if (slot < 2 || slot > SPEC_COUNT || !player->HasSpell(SPEC_SWAP_SPELLS[slot - 1]))
        return false;

    player->removeSpell(SPEC_SWAP_SPELLS[slot - 1], SPEC_MASK_ALL, false);
    return true;
}

/* ------------------------------------------------------------------ *
 * Startup self-check                                                  *
 * ------------------------------------------------------------------ */

/* The table above is the client's (`Constants.lua`), and the realm reads its own copy of the same
 * spells out of `Spell.dbc`. Both halves of the contract are checked here, once, after the spell
 * data is up: the slot's spell exists, it is an effect-162 spell in effect slot one, its own
 * `EffectBasePoints_1` field is the zero-based slot the client's table was built from, and the value
 * the engine will hand the effect handler for it is the one-based slot the switch works in. A
 * disagreement is a data fault that would otherwise show up as "the spec list does nothing" or as a
 * switch to the wrong slot. */
void CheckSwapSpells()
{
    uint32 broken = 0;
    for (uint32 slot = 1; slot <= SPEC_COUNT; ++slot)
    {
        uint32 const spellId = SPEC_SWAP_SPELLS[slot - 1];
        SpellInfo const *info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
        {
            Report(0, Acore::StringFormat("swap spell {} of {} does not exist in the spell data",
                spellId, SpecName(slot)));
            ++broken;
            continue;
        }

        SpellEffectInfo const &effect = info->Effects[EFFECT_0];
        int32 const value = effect.CalcValue();
        if (effect.Effect != SPELL_EFFECT_TALENT_SPEC_SELECT || effect.BasePoints != int32(slot) - 1
            || value != int32(slot))
        {
            Report(0, Acore::StringFormat("swap spell {} of {} is effect {} with payload {} and "
                "value {}; slot {} needs effect {} with payload {} and value {}", spellId,
                SpecName(slot), uint32(effect.Effect), effect.BasePoints, value, slot,
                uint32(SPELL_EFFECT_TALENT_SPEC_SELECT), slot - 1, slot));
            ++broken;
        }
    }

    Report(1, Acore::StringFormat("swap spell table checked: {}/{} spells agree with the client's slots",
        SPEC_COUNT - broken, SPEC_COUNT));
}

/* ------------------------------------------------------------------ *
 * Scripts                                                            *
 * ------------------------------------------------------------------ */

/* The module is registered even when it is switched off, so the spell_script_names rows the
 * database holds always resolve; every handler below stands down on its own when the module is
 * off, and the cast then keeps whatever behavior it has without a script. */

/* The swap-spell table is checked against the loaded spell data, so the check belongs to world
 * startup and not to `Register()`: module scripts are registered before `sSpellMgr` has its spells,
 * and a check there reads an empty spell store and reports twenty missing spells that are all
 * present. */
class TomeOfSpecWorldScript : public WorldScript
{
public:
    TomeOfSpecWorldScript() : WorldScript("tomeofspec_world", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        CheckSwapSpells();
    }
};
class TomeOfSpecPlayerScript : public PlayerScript
{
public:
    TomeOfSpecPlayerScript()
        : PlayerScript("tomeofspec_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEARN_SPELL }) { }

    void OnPlayerLogin(Player *player) override
    {
        if (!Owns(player))
            return;

        /* The base slot's spell is not implied by "Spec I is always unlocked": the client's own
         * button is enabled by `IsSpellKnown`, and its click casts the spell, so a character
         * without it can never return to slot one. */
        if (Cfg::Flag(Cfg::GrantStartingSlot, true) && GrantSlot(player, 1))
            Report(1, Acore::StringFormat("{} had no base slot spell; granted {}",
                player->GetName(), SPEC_SWAP_SPELLS[0]));

        NormalizeActiveSlot(player);

        uint32 const slot = CurrentSlot(player);
        SlotState const state = LoadSlot(player, slot);
        Report(1, Acore::StringFormat("{} is on slot {} of {} (archetype {}, {} stored row(s))",
            player->GetName(), slot, UnlockedCount(player), GetAscensionActiveSpecialization(player),
            state.Configured ? state.Entries.size() : 0));
    }

    void OnPlayerLearnSpell(Player *player, uint32 spellId) override
    {
        if (!Owns(player))
            return;

        for (uint32 slot = 2; slot <= SPEC_COUNT; ++slot)
        {
            if (SPEC_SWAP_SPELLS[slot - 1] != spellId)
                continue;

            if (Cfg::Flag(Cfg::AnnounceUnlocks, true))
                ChatHandler(player->GetSession()).SendSysMessage(
                    Acore::StringFormat("{} is now open to you. Click it in your specialization list to use it.",
                        SpecName(slot)));

            /* Only the slot half of the push: a Tome changes which slots exist, not the build the
             * character holds, and the entries packet resets the client's pending build. */
            PushAscensionActiveSpec(player);
            Report(1, Acore::StringFormat("{} unlocked slot {} ({}/{} open)", player->GetName(), slot,
                UnlockedCount(player), SPEC_COUNT));
            return;
        }
    }
};

/* The player's click, on the wire: the client's spec list casts the slot's swap spell
 * (`SpecializationUtil.InternalActivateSpecialization`), and that cast is what arrives here. The
 * click is therefore a cast the player has to finish, not a menu action - the client's own spells
 * carry a five second, movement/damage-interrupted cast time (`CastingTimeIndex` 6, 5000 ms), which
 * is why its list refuses to start one while the character is moving, and this handler runs when
 * that cast completes. Interrupting it leaves the character on the slot it was on, as it should.
 *
 * The default effect is prevented in every case - it is the vanilla two-spec activation, which would
 * switch talent groups the archetype system does not use, and letting it run beside this one would
 * be the second switch that owns the same character. */
class spell_tomeofspec_slot_switch : public SpellScript
{
    PrepareSpellScript(spell_tomeofspec_slot_switch);

    void Switch(SpellEffIndex index)
    {
        if (!Cfg::Enabled())
            return;

        Player *player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !Owns(player))
            return;

        PreventHitDefaultEffect(index);

        /* The slot comes from which spell was cast, never from what the spell carries. The database
         * binds this script to the twenty swap spells only, so a binding to anything else is a data
         * fault: it is reported, and the vanilla activation stays stood down rather than being
         * handed a character the slot layer does not own. */
        uint32 const slot = GetSpellInfo() ? SlotForSpell(GetSpellInfo()->Id) : 0;
        if (!slot)
        {
            Report(0, Acore::StringFormat("{} cast {} as a slot switch, but it is not one of the "
                "client's swap spells", player->GetName(), GetSpellInfo() ? GetSpellInfo()->Id : 0));
            return;
        }

        int32 const payload = GetEffectValue();
        if (payload != int32(slot))
            Report(0, Acore::StringFormat("swap spell {} of {} carries value {} instead of {}: the "
                "spell data and the client's table disagree", GetSpellInfo()->Id, SpecName(slot),
                payload, slot));

        std::string refusal;
        if (!SwitchToSlot(player, slot, &refusal))
        {
            /* The character stays on the slot it is on; the push re-affirms it so a client that
             * mis-drew the click is corrected from the realm's answer. */
        }

        if (!refusal.empty())
        {
            Report(0, Acore::StringFormat("refused {} on slot {}: {}", player->GetName(), slot, refusal));
            if (player->IsInWorld())
                ChatHandler(player->GetSession()).SendSysMessage(refusal);
            PushAscensionActiveSpec(player);
            return;
        }

        PushSlotState(player, "slot switch");
        AnnounceSlot(player, CurrentSlot(player));
        Report(1, Acore::StringFormat("{} switched to slot {} (archetype {})", player->GetName(),
            CurrentSlot(player), GetAscensionActiveSpecialization(player)));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_tomeofspec_slot_switch::Switch, EFFECT_0,
            SPELL_EFFECT_TALENT_SPEC_SELECT);
    }
};

/* ------------------------------------------------------------------ *
 * The command table                                                   *
 * ------------------------------------------------------------------ *
 * The cast above is the player's own path and cannot be faked from the server console, so the same
 * routine is reachable by name: an administrator can grant or revoke a slot, select one, or point
 * a slot at an archetype without a client at all. */
class TomeOfSpecCommandScript : public CommandScript
{
public:
    TomeOfSpecCommandScript() : CommandScript("TomeOfSpecCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable const slotCommands =
        {
            { "grant",      HandleGrant,      SEC_ADMINISTRATOR, Console::No },
            { "revoke",     HandleRevoke,     SEC_ADMINISTRATOR, Console::No },
            { "select",     HandleSelect,     SEC_ADMINISTRATOR, Console::No },
            { "setspec",    HandleSetSpec,    SEC_ADMINISTRATOR, Console::No },
            { "clearslots", HandleClearSlots, SEC_ADMINISTRATOR, Console::No },
            { "status",     HandleStatus,     SEC_ADMINISTRATOR, Console::No },
        };
        static ChatCommandTable const commands =
        {
            { "tomeofspec", slotCommands }
        };
        return commands;
    }

    static Player *Selected(ChatHandler *handler)
    {
        Player *player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            handler->SendErrorMessage("This command needs a player session.");
        else if (!Owns(player))
            handler->SendErrorMessage("Tome of Specialization is not enabled for this character "
                "(TomeOfSpec.Enable, custom class).");

        return player;
    }

    static bool HandleGrant(ChatHandler *handler, std::string argument)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        if (argument == "all")
        {
            uint32 granted = 0;
            for (uint32 slot = 1; slot <= SPEC_COUNT; ++slot)
                if (GrantSlot(player, slot))
                    ++granted;

            handler->PSendSysMessage("mod-tomeofspec: granted {} slot(s); {}/{} are now open.",
                granted, UnlockedCount(player), SPEC_COUNT);
            return true;
        }

        uint32 const slot = uint32(strtoul(argument.c_str(), nullptr, 10));
        if (slot < 1 || slot > SPEC_COUNT)
        {
            handler->SendErrorMessage("Usage: .tomeofspec grant <1-{}|all>", SPEC_COUNT);
            return true;
        }

        handler->PSendSysMessage("mod-tomeofspec: {} {} (spell {}), {}/{} open.", SpecName(slot),
            GrantSlot(player, slot) ? "granted" : "was already known",
            SPEC_SWAP_SPELLS[slot - 1], UnlockedCount(player), SPEC_COUNT);
        return true;
    }

    static bool HandleRevoke(ChatHandler *handler, uint32 slot)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        if (slot < 2 || slot > SPEC_COUNT)
        {
            handler->SendErrorMessage("Usage: .tomeofspec revoke <2-{}>", SPEC_COUNT);
            return true;
        }

        bool const revoked = RevokeSlot(player, slot);
        /* A revoked slot the character was on is not a slot it can stay on: the fallback takes up the
         * first slot's own preset (build and bar), and the state push names the slot the character
         * now holds. */
        NormalizeActiveSlot(player);
        PushSlotState(player, "revoke");
        handler->PSendSysMessage("mod-tomeofspec: {} {}; {}/{} open, on slot {}.", SpecName(slot),
            revoked ? "revoked" : "was not known", UnlockedCount(player), SPEC_COUNT,
            CurrentSlot(player));
        return true;
    }

    static bool HandleSelect(ChatHandler *handler, uint32 slot)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        std::string refusal;
        if (!SwitchToSlot(player, slot, &refusal))
        {
            handler->SendErrorMessage("mod-tomeofspec: {}", refusal);
            return true;
        }

        PushSlotState(player, "command");
        handler->PSendSysMessage("mod-tomeofspec: on slot {} ({}), archetype {}.", CurrentSlot(player),
            SpecName(CurrentSlot(player)), GetAscensionActiveSpecialization(player));
        return true;
    }

    /* Points a slot at an archetype, with that archetype's own defaults as its build: the shape an
     * operator needs after content moved a character's class or an archetype's tree, and the only
     * way to fill a slot without a client. */
    static bool HandleSetSpec(ChatHandler *handler, uint32 slot, uint32 specId)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        if (slot < 1 || slot > SPEC_COUNT)
        {
            handler->SendErrorMessage("Usage: .tomeofspec setspec <1-{}> <archetype id>", SPEC_COUNT);
            return true;
        }

        AscensionCompatData::CoASpecialization const *found = nullptr;
        for (AscensionCompatData::CoASpecialization const &spec : AscensionCompatData::CoASpecializations)
            if (spec.SpecId == specId && spec.ClassId == player->getClass())
            {
                found = &spec;
                break;
            }

        if (!found)
        {
            handler->SendErrorMessage("mod-tomeofspec: archetype {} is not one of this character's class.",
                specId);
            return true;
        }

        StoreSlotRecord(player, slot, specId,
            AscensionCoATalentState::DefaultEntries(player->getClass(), specId));
        handler->PSendSysMessage("mod-tomeofspec: slot {} now holds archetype {} and its {} default row(s).",
            slot, specId, AscensionCoATalentState::DefaultEntries(player->getClass(), specId).size());
        return true;
    }

    static bool HandleClearSlots(ChatHandler *handler)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        for (uint32 slot = 1; slot <= SPEC_COUNT; ++slot)
        {
            for (std::string const &setting : { SlotSetting(slot), BarSetting(slot) })
            {
                std::size_t const previous = SettingValues(player, setting).size();
                for (std::size_t index = 0; index < previous; ++index)
                    player->UpdatePlayerSetting(setting, uint32(index), 0);
            }
        }

        /* The records are what is cleared; the character keeps the build it is holding until the
         * next switch, which is what makes the command a way back from a bad record and not a way
         * to wipe a character. */
        StoreSlot(player, 1);
        PushAscensionActiveSpec(player);
        handler->PSendSysMessage("mod-tomeofspec: every slot record and bar cleared for {}, slot one is "
            "active.", player->GetName());
        return true;
    }

    static bool HandleStatus(ChatHandler *handler)
    {
        Player *player = Selected(handler);
        if (!player)
            return true;

        handler->PSendSysMessage("mod-tomeofspec: class {}, level {}, on slot {} of {} ({} row(s) held, "
            "{} bar button(s), archetype {}).", uint32(player->getClass()), uint32(player->GetLevel()),
            CurrentSlot(player), UnlockedCount(player), GetAscensionTalentState(player).size(),
            LiveBar(player).Buttons.size(), GetAscensionActiveSpecialization(player));

        for (uint32 slot = 1; slot <= SPEC_COUNT; ++slot)
        {
            SlotState const state = LoadSlot(player, slot);
            /* The live bar stands in for the active slot's record: it is the same arrangement, read
             * from the character instead of from storage. */
            uint32 const barCount = slot == CurrentSlot(player)
                ? uint32(LiveBar(player).Buttons.size()) : uint32(LoadSlotBar(player, slot).Buttons.size());
            handler->PSendSysMessage("  {} {}: {}", Unlocked(player, slot) ? "open  " : "locked",
                SpecName(slot), state.Configured
                    ? Acore::StringFormat("archetype {}, {} row(s), {} bar button(s)", state.SpecId,
                        state.Entries.size(), barCount)
                    : Acore::StringFormat("never configured, {} bar button(s)", barCount));
        }

        return true;
    }
};

/* ------------------------------------------------------------------ *
 * Registration                                                        *
 * ------------------------------------------------------------------ */
void Register()
{
    /* Registered whether or not the module is on: the spell names the database binds are resolved
     * either way, and each handler decides for itself. */
    new TomeOfSpecPlayerScript();
    new TomeOfSpecWorldScript();
    new TomeOfSpecCommandScript();
    RegisterSpellScriptWithArgs(spell_tomeofspec_slot_switch, "spell_tomeofspec_slot_switch");

    if (!Cfg::Enabled())
    {
        LOG_INFO("coa", MOD_TOMEOFSPEC_LOG_TAG "TomeOfSpec.Enable = 0: standing down; no slot provider is "
            "registered and a swap-spell cast keeps its own behavior.");
        return;
    }

    SetAscensionActiveSpecSlotProvider(&AnswerActiveSpec);
    LOG_INFO("coa", MOD_TOMEOFSPEC_LOG_TAG "tome of specialization {} (starting slot grant {}, unlock "
        "announcements {}, log level {})", "ENABLED",
        Cfg::Flag(Cfg::GrantStartingSlot, true) ? "on" : "off",
        Cfg::Flag(Cfg::AnnounceUnlocks, true) ? "on" : "off",
        Cfg::Number(Cfg::LogLevel, 1));
}

} // namespace TomeOfSpec

/* The generated module loader derives this name from the folder:
 * "mod-tomeofspec" -> Addmod_tomeofspecScripts */
void Addmod_tomeofspecScripts()
{
    TomeOfSpec::Register();
}
