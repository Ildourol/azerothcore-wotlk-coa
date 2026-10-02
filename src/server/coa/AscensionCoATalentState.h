#ifndef ASCENSION_COA_TALENT_STATE_H
#define ASCENSION_COA_TALENT_STATE_H

#include "AscensionCoATalentData.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AscensionCoATalentState
{
using HasSpell = std::function<bool(std::uint32_t)>;

struct KnownEntry
{
    std::uint32_t EntryId;
    std::uint32_t Rank;
    std::uint32_t LearnedSpellRank = 0;
    bool Locked = false;
    std::uint32_t LearnOrder = 0;
};

std::uint32_t KnownRank(AscensionCompatData::CoATalentEntry const& entry, HasSpell const& hasSpell);

std::vector<KnownEntry> KnownEntries(std::uint8_t classId, HasSpell const& hasSpell);

struct SpentPoints
{
    std::uint32_t AE = 0;
    std::uint32_t TE = 0;
};

std::vector<KnownEntry> DefaultEntries(std::uint8_t classId, std::uint32_t specId);
bool IsDefaultEntry(std::uint8_t classId, std::uint32_t specId, std::uint32_t entryId);
SpentPoints Spent(std::vector<KnownEntry> const& known, std::uint8_t classId = 0, std::uint32_t specId = 0);
std::vector<std::uint32_t> SelectedSpells(std::vector<KnownEntry> const& known);

std::vector<std::uint32_t> UnsatisfiedInvestmentGates(std::vector<KnownEntry> const& known);

std::string InvestmentGateShortfall(std::uint32_t entryId, std::vector<KnownEntry> const& known);

std::vector<std::uint8_t> KnownEntriesPayload(std::vector<KnownEntry> const& known);

bool ParseKnownEntriesUpload(std::uint8_t const* data, std::size_t size, std::vector<KnownEntry>& known);

struct UploadedSpecialization
{
    std::uint32_t SpecId = 0;
    std::uint32_t IdentitySpecId = 0;
    bool Mixed = false;
    bool ChoosesTalents = false;
};

constexpr std::uint32_t TalentStateRevision = 4;

constexpr std::size_t LoadoutMaxEntries = 32;

std::uint32_t TalentStateStride(std::uint32_t revision);

struct Loadout
{
    std::string Uuid;
    std::string Name;
    std::uint32_t SortOrder = 0;
    std::uint32_t SpecId = 0;
    std::vector<KnownEntry> Entries;
};

constexpr std::size_t MaxLoadouts = 24;
constexpr std::size_t LoadoutUuidBytes = 40;
constexpr std::size_t LoadoutNameBytes = 64;
constexpr std::size_t LoadoutUuidSlots = LoadoutUuidBytes / 4;
constexpr std::size_t LoadoutNameSlots = LoadoutNameBytes / 4;
constexpr std::size_t LoadoutRecordSlots = 2 + LoadoutUuidSlots + LoadoutNameSlots + 2;
constexpr std::size_t LoadoutRecordEntriesSlots = 1 + LoadoutMaxEntries * 3;
constexpr std::size_t LoadoutRecordSlotsV4 = LoadoutRecordSlots + LoadoutRecordEntriesSlots;
constexpr std::size_t LoadoutActiveSlots = 1 + LoadoutUuidSlots;

constexpr std::size_t ClampLoadoutEntries(std::size_t entries)
{
    return entries > LoadoutMaxEntries ? LoadoutMaxEntries : entries;
}

std::size_t LoadoutRecordStride(std::uint32_t revision);

std::size_t LoadoutBlockSlots(std::size_t count);
std::size_t LoadoutBlockSlots(std::size_t count, std::size_t recordSlots);

std::vector<std::uint32_t> BuildLoadoutBlock(std::vector<Loadout> const& loadouts,
                                             std::string const& activeUuid);

bool ParseLoadoutBlock(std::uint32_t const* slots, std::size_t size, std::size_t recordSlots,
                       std::vector<Loadout>& loadouts, std::string& activeUuid);

UploadedSpecialization SpecializationOf(std::vector<KnownEntry> const& upload);

std::vector<KnownEntry> SpecializationSwitch(std::uint8_t classId, HasSpell const& hasSpell, std::uint32_t specId);
}

#endif
