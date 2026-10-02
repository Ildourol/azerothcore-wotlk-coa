#ifndef ASCENSION_COA_TALENT_DATA_H
#define ASCENSION_COA_TALENT_DATA_H

#include <array>
#include <cstdint>
#include <vector>

namespace AscensionCompatData
{
enum CoAEntryType : std::uint8_t
{
    COA_ENTRY_NONE = 0,
    COA_ENTRY_ABILITY = 1,
    COA_ENTRY_TALENT = 2,
    COA_ENTRY_TRAIT = 3,
    COA_ENTRY_TALENT_ABILITY = 4
};

struct CoATalentEntry
{
    std::uint32_t EntryId;
    std::uint8_t ClassId;
    std::uint16_t SpecId;
    std::uint8_t SpellCount;
    std::uint8_t AECost;
    std::uint8_t TECost;
    std::uint8_t RequiredLevel;
    std::uint8_t Type = COA_ENTRY_NONE;
    std::uint8_t TabId;
    std::array<std::uint16_t, 3> GateAE;
    std::array<std::uint16_t, 3> GateTE;
    std::uint16_t PointsGate;
    std::uint16_t Points;
    std::array<std::uint32_t, 3> SpellIds;
};

struct CoASelectableFreeEntry
{
    std::uint32_t EntryId;
    std::uint32_t GroupId;
};

struct CoAAutomaticDependency
{
    std::uint32_t EntryId;
    std::array<std::uint32_t, 2> RequiredEntryIds;
};

struct CoASpecialization
{
    std::uint16_t SpecId;
    std::uint8_t ClassId;
    std::uint32_t IdentityEntryId;
    std::uint32_t SignatureEntryId;
    std::vector<std::uint32_t> OpeningRowEntryIds;
};

struct CoATalentBudget
{
    std::uint8_t ClassId;
    std::uint8_t Level;
    std::uint8_t AE;
    std::uint8_t TE;
};

inline bool IsAbilityLike(CoATalentEntry const& entry)
{
    return entry.Type == COA_ENTRY_ABILITY || entry.Type == COA_ENTRY_TALENT_ABILITY;
}

inline bool IsTalent(CoATalentEntry const& entry)
{
    return entry.Type == COA_ENTRY_TALENT;
}

extern std::vector<CoATalentEntry> CoATalentEntries;
extern std::vector<CoASelectableFreeEntry> CoASelectableFreeEntries;
extern std::vector<CoAAutomaticDependency> CoAAutomaticDependencies;
extern std::vector<CoASpecialization> CoASpecializations;
extern std::vector<CoATalentBudget> CoATalentBudgets;

bool GetCoATalentBudget(std::uint8_t classId, std::uint8_t level, std::uint32_t& ae, std::uint32_t& te);

bool IsCoAFreeToUnlearnRow(std::uint32_t entryId);

bool IsCoAGrantedRow(std::uint32_t entryId);

bool LoadCoATalentData();
}

#endif
