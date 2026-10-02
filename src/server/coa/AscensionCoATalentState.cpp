#include "AscensionCoATalentState.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace AscensionCoATalentState
{
namespace
{
constexpr std::size_t RECORD_SIZE = 21;

void AppendUInt32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(std::uint8_t(value >> shift));
}

std::uint32_t ReadUInt32(std::uint8_t const* data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
        (std::uint32_t(data[3]) << 24);
}

AscensionCompatData::CoATalentEntry const* FindEntry(std::uint32_t entryId)
{
    auto const& entries = AscensionCompatData::CoATalentEntries;
    auto itr = std::lower_bound(entries.begin(), entries.end(), entryId,
        [](AscensionCompatData::CoATalentEntry const& entry, std::uint32_t id) { return entry.EntryId < id; });
    return itr != entries.end() && itr->EntryId == entryId ? &*itr : nullptr;
}

void WriteStringSlots(std::vector<std::uint32_t>& slots, std::size_t base, std::string const& text,
                      std::size_t maxBytes)
{
    std::size_t const length = std::min(text.size(), maxBytes);
    slots[base] = std::uint32_t(length);
    for (std::size_t slot = 0; slot < (maxBytes + 3) / 4; ++slot)
    {
        std::uint32_t packed = 0;
        for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte)
            if (std::size_t const index = slot * sizeof(std::uint32_t) + byte; index < length)
                packed |= std::uint32_t(std::uint8_t(text[index])) << std::uint32_t(8 * byte);
        slots[base + 1 + slot] = packed;
    }
}

bool ReadStringSlots(std::uint32_t const* slots, std::size_t size, std::size_t base,
                     std::size_t maxBytes, std::string& text)
{
    if (base >= size)
        return false;
    std::size_t const length = std::min<std::size_t>(slots[base], maxBytes);
    if (size < base + 1 + (length + 3) / 4)
        return false;
    text.assign(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
        text[index] = char(std::uint8_t(slots[base + 1 + index / 4] >> std::uint32_t(8 * (index % 4))));
    return true;
}

bool IsSelectableFree(std::uint32_t entryId)
{
    return std::any_of(AscensionCompatData::CoASelectableFreeEntries.begin(),
        AscensionCompatData::CoASelectableFreeEntries.end(),
        [entryId](AscensionCompatData::CoASelectableFreeEntry const& entry) { return entry.EntryId == entryId; });
}

bool IsIdentity(std::uint32_t entryId)
{
    return std::any_of(AscensionCompatData::CoASpecializations.begin(),
        AscensionCompatData::CoASpecializations.end(),
        [entryId](AscensionCompatData::CoASpecialization const& specialization)
        {
            return specialization.IdentityEntryId == entryId;
        });
}

std::uint32_t InvestmentSum(std::vector<KnownEntry> const& known, bool te, std::size_t scope,
                            std::uint32_t minimum, std::uint8_t classId, std::uint8_t tabId)
{
    std::uint32_t total = 0;
    for (KnownEntry const& item : known)
    {
        AscensionCompatData::CoATalentEntry const* entry = FindEntry(item.EntryId);
        if (!entry)
            continue;
        if (minimum && (te ? entry->GateTE[scope] : entry->GateAE[scope]) >= minimum)
            continue;
        if (scope >= 1 && entry->ClassId != classId)
            continue;
        if (scope == 2 && entry->TabId != tabId)
            continue;
        total += std::uint32_t(te ? entry->TECost : entry->AECost) * item.Rank;
    }
    return total;
}

std::uint32_t InvestmentPoints(std::vector<KnownEntry> const& known, std::uint32_t minimum, std::uint8_t classId)
{
    std::uint32_t total = 0;
    for (KnownEntry const& item : known)
    {
        AscensionCompatData::CoATalentEntry const* entry = FindEntry(item.EntryId);
        if (!entry || (minimum && entry->PointsGate >= minimum) || entry->ClassId != classId)
            continue;
        total += std::uint32_t(entry->Points) * item.Rank;
    }
    return total;
}
}

std::string InvestmentGateShortfall(std::uint32_t entryId, std::vector<KnownEntry> const& known)
{
    AscensionCompatData::CoATalentEntry const* entry = FindEntry(entryId);
    if (!entry)
        return {};

    static char const* const scopes[3] = { "the whole build", "the class tree", "its own tree" };
    for (std::size_t scope = 0; scope < 3; ++scope)
    {
        std::uint32_t const ae = entry->GateAE[scope];
        if (ae)
        {
            std::uint32_t const got = InvestmentSum(known, false, scope, ae, entry->ClassId, entry->TabId);
            if (got < ae)
                return "needs " + std::to_string(ae) + " class point(s) invested in " + scopes[scope] +
                       " first, the build has " + std::to_string(got);
        }
        std::uint32_t const te = entry->GateTE[scope];
        if (te)
        {
            std::uint32_t const got = InvestmentSum(known, true, scope, te, entry->ClassId, entry->TabId);
            if (got < te)
                return "needs " + std::to_string(te) + " specialization point(s) invested in " + scopes[scope] +
                       " first, the build has " + std::to_string(got);
        }
    }
    std::uint32_t const points = entry->PointsGate;
    if (points)
    {
        std::uint32_t const got = InvestmentPoints(known, points, entry->ClassId);
        if (got < points)
            return "needs " + std::to_string(points) + " talent point(s) invested in its class first, the build has " +
                   std::to_string(got);
    }
    return {};
}

std::vector<std::uint32_t> UnsatisfiedInvestmentGates(std::vector<KnownEntry> const& known)
{
    std::vector<std::uint32_t> unsatisfied;
    for (KnownEntry const& item : known)
        if (item.Rank && !InvestmentGateShortfall(item.EntryId, known).empty())
            unsatisfied.push_back(item.EntryId);
    return unsatisfied;
}

std::uint32_t KnownRank(AscensionCompatData::CoATalentEntry const& entry, HasSpell const& hasSpell)
{
    std::uint32_t rank = 0;
    for (std::uint32_t index = 0; index < entry.SpellCount; ++index)
        if (entry.SpellIds[index] && hasSpell(entry.SpellIds[index]))
            rank = index + 1;
    return rank;
}

std::vector<KnownEntry> KnownEntries(std::uint8_t classId, HasSpell const& hasSpell)
{
    std::vector<AscensionCompatData::CoATalentEntry const*> entries;
    for (AscensionCompatData::CoATalentEntry const& entry : AscensionCompatData::CoATalentEntries)
        if (entry.ClassId == classId)
            entries.push_back(&entry);
    std::stable_sort(entries.begin(), entries.end(),
        [](AscensionCompatData::CoATalentEntry const* left, AscensionCompatData::CoATalentEntry const* right)
        { return left->SpecId < right->SpecId; });

    std::unordered_map<std::uint32_t, std::uint16_t> ownerSpec;
    std::vector<KnownEntry> known;
    for (AscensionCompatData::CoATalentEntry const* entry : entries)
    {
        std::uint32_t const rank = KnownRank(*entry, hasSpell);
        if (!rank)
            continue;

        bool claimed = false;
        for (std::uint32_t index = 0; index < rank && !claimed; ++index)
        {
            auto const owner = ownerSpec.find(entry->SpellIds[index]);
            claimed = owner != ownerSpec.end() && owner->second != entry->SpecId;
        }
        if (claimed)
            continue;

        for (std::uint32_t index = 0; index < rank; ++index)
            ownerSpec.emplace(entry->SpellIds[index], entry->SpecId);
        known.push_back({ entry->EntryId, rank });
    }
    return known;
}

std::vector<KnownEntry> DefaultEntries(std::uint8_t classId, std::uint32_t specId)
{
    for (auto const& spec : AscensionCompatData::CoASpecializations)
    {
        if (spec.ClassId != classId || spec.SpecId != specId || !spec.SignatureEntryId || !spec.IdentityEntryId)
            continue;
        std::vector<KnownEntry> defaults = { { spec.SignatureEntryId, 1 }, { spec.IdentityEntryId, 1 } };
        for (std::uint32_t openingId : spec.OpeningRowEntryIds)
            if (openingId && std::none_of(defaults.begin(), defaults.end(),
                [openingId](KnownEntry const& item) { return item.EntryId == openingId; }))
                defaults.push_back({ openingId, 1 });

        for (bool added = true; added;)
        {
            added = false;
            for (auto const& dependency : AscensionCompatData::CoAAutomaticDependencies)
            {
                auto const* entry = FindEntry(dependency.EntryId);
                if (!entry || entry->ClassId != classId)
                    continue;
                if (!AscensionCompatData::IsCoAGrantedRow(dependency.EntryId))
                    continue;
                if (entry->SpecId && entry->SpecId != specId)
                    continue;
                if (std::any_of(defaults.begin(), defaults.end(),
                    [&dependency](KnownEntry const& item) { return item.EntryId == dependency.EntryId; }))
                    continue;
                bool satisfied = true;
                for (std::uint32_t requiredId : dependency.RequiredEntryIds)
                    if (requiredId && std::none_of(defaults.begin(), defaults.end(),
                        [requiredId](KnownEntry const& item) { return item.EntryId == requiredId; }))
                    {
                        satisfied = false;
                        break;
                    }
                if (!satisfied)
                    continue;
                defaults.push_back({ dependency.EntryId, 1 });
                added = true;
            }
        }
        return defaults;
    }
    return {};
}

bool IsDefaultEntry(std::uint8_t classId, std::uint32_t specId, std::uint32_t entryId)
{
    auto const defaults = DefaultEntries(classId, specId);
    return std::any_of(defaults.begin(), defaults.end(),
        [entryId](KnownEntry const& item) { return item.EntryId == entryId; });
}

std::vector<std::uint32_t> SelectedSpells(std::vector<KnownEntry> const& known)
{
    std::vector<std::uint32_t> spells;
    for (KnownEntry const& item : known)
        if (auto const* entry = FindEntry(item.EntryId); entry && item.Rank && item.Rank <= entry->SpellCount)
            if (std::uint32_t const spellId = entry->SpellIds[item.Rank - 1])
                spells.push_back(spellId);
    std::sort(spells.begin(), spells.end());
    spells.erase(std::unique(spells.begin(), spells.end()), spells.end());
    return spells;
}

SpentPoints Spent(std::vector<KnownEntry> const& known, std::uint8_t classId, std::uint32_t specId)
{
    SpentPoints spent;
    std::unordered_set<std::uint32_t> charged;
    for (auto const& item : DefaultEntries(classId, specId))
        if (auto const* entry = FindEntry(item.EntryId); entry && entry->SpellCount)
            charged.insert(entry->SpellIds[0]);
    for (KnownEntry const& item : known)
    {
        AscensionCompatData::CoATalentEntry const* entry = FindEntry(item.EntryId);
        if (!entry || (!entry->AECost && !entry->TECost))
            continue;

        std::uint32_t const ranks = std::min<std::uint32_t>(item.Rank, entry->SpellCount);
        for (std::uint32_t index = 0; index < ranks; ++index)
        {
            if (!charged.insert(entry->SpellIds[index]).second)
                continue;
            if (entry->SpecId)
                spent.TE += entry->TECost;
            else
                spent.AE += entry->AECost;
        }
    }
    return spent;
}

std::vector<std::uint8_t> KnownEntriesPayload(std::vector<KnownEntry> const& known)
{
    std::vector<std::uint8_t> out;
    out.reserve(sizeof(std::uint32_t) + known.size() * RECORD_SIZE);
    AppendUInt32(out, std::uint32_t(known.size()));
    for (KnownEntry const& item : known)
    {
        AppendUInt32(out, item.EntryId);
        AppendUInt32(out, item.Rank);
        AppendUInt32(out, item.LearnedSpellRank);
        out.push_back(item.Locked ? 1 : 0);
        AppendUInt32(out, item.LearnOrder);
        AppendUInt32(out, 0);
    }
    return out;
}

bool ParseKnownEntriesUpload(std::uint8_t const* data, std::size_t size, std::vector<KnownEntry>& known)
{
    known.clear();
    if (!data || size < sizeof(std::uint32_t))
        return false;

    std::uint32_t const count = ReadUInt32(data);
    if (size != sizeof(std::uint32_t) + std::size_t(count) * RECORD_SIZE)
        return false;

    known.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint8_t const* record = data + sizeof(std::uint32_t) + std::size_t(index) * RECORD_SIZE;
        known.push_back({ ReadUInt32(record), ReadUInt32(record + 4), 0, record[12] != 0, 0 });
    }
    return true;
}

UploadedSpecialization SpecializationOf(std::vector<KnownEntry> const& upload)
{
    UploadedSpecialization uploaded;
    std::vector<AscensionCompatData::CoATalentEntry const*> chosen;
    for (KnownEntry const& item : upload)
    {
        AscensionCompatData::CoATalentEntry const* entry = item.Rank ? FindEntry(item.EntryId) : nullptr;
        if (!entry)
            continue;
        if (IsIdentity(entry->EntryId))
            uploaded.IdentitySpecId = entry->SpecId;
        if (!entry->SpecId)
            continue;
        if (uploaded.SpecId && uploaded.SpecId != entry->SpecId)
            uploaded.Mixed = true;
        else
            uploaded.SpecId = entry->SpecId;
        if (!IsIdentity(entry->EntryId))
            chosen.push_back(entry);
    }

    AscensionCompatData::CoASpecialization const* signature = nullptr;
    for (AscensionCompatData::CoASpecialization const& specialization : AscensionCompatData::CoASpecializations)
        if (specialization.SpecId == uploaded.SpecId)
            signature = &specialization;
    uploaded.ChoosesTalents = std::any_of(chosen.begin(), chosen.end(),
        [signature](AscensionCompatData::CoATalentEntry const* entry)
        {
            if (!signature || entry->EntryId == signature->SignatureEntryId ||
                entry->EntryId == signature->IdentityEntryId)
                return false;
            return std::none_of(signature->OpeningRowEntryIds.begin(), signature->OpeningRowEntryIds.end(),
                [entry](std::uint32_t openingId) { return openingId == entry->EntryId; });
        });
    return uploaded;
}

std::uint32_t TalentStateStride(std::uint32_t revision)
{
    return revision == 4 || revision == 3 || revision == 2 ? 3 : revision == 1 ? 2 : 0;
}

std::size_t LoadoutRecordStride(std::uint32_t revision)
{
    return revision == 4 ? LoadoutRecordSlotsV4 : revision == 3 ? LoadoutRecordSlots : 0;
}

std::size_t LoadoutBlockSlots(std::size_t count, std::size_t recordSlots)
{
    return 1 + count * recordSlots + LoadoutActiveSlots;
}

std::size_t LoadoutBlockSlots(std::size_t count)
{
    return LoadoutBlockSlots(count, LoadoutRecordSlotsV4);
}

std::vector<std::uint32_t> BuildLoadoutBlock(std::vector<Loadout> const& loadouts,
                                             std::string const& activeUuid)
{
    std::size_t const count = std::min(loadouts.size(), MaxLoadouts);
    std::vector<std::uint32_t> slots(LoadoutBlockSlots(count), 0);
    slots[0] = std::uint32_t(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        std::size_t const record = 1 + index * LoadoutRecordSlotsV4;
        WriteStringSlots(slots, record, loadouts[index].Uuid, LoadoutUuidBytes);
        std::size_t const name = record + 1 + LoadoutUuidSlots;
        WriteStringSlots(slots, name, loadouts[index].Name, LoadoutNameBytes);
        slots[name + 1 + LoadoutNameSlots] = loadouts[index].SortOrder;
        slots[name + 2 + LoadoutNameSlots] = loadouts[index].SpecId;
        std::size_t const entries = name + 3 + LoadoutNameSlots;
        std::size_t const stored = ClampLoadoutEntries(loadouts[index].Entries.size());
        slots[entries] = std::uint32_t(stored);
        for (std::size_t item = 0; item < stored; ++item)
        {
            slots[entries + 1 + item * 3] = loadouts[index].Entries[item].EntryId;
            slots[entries + 2 + item * 3] = loadouts[index].Entries[item].Rank;
            slots[entries + 3 + item * 3] = loadouts[index].Entries[item].Locked ? 1u : 0u;
        }
    }
    WriteStringSlots(slots, 1 + count * LoadoutRecordSlotsV4, activeUuid, LoadoutUuidBytes);
    return slots;
}

bool ParseLoadoutBlock(std::uint32_t const* slots, std::size_t size, std::size_t recordSlots,
                       std::vector<Loadout>& loadouts, std::string& activeUuid)
{
    loadouts.clear();
    activeUuid.clear();
    if (!slots || !size || !recordSlots)
        return false;
    std::size_t const count = std::min<std::size_t>(slots[0], MaxLoadouts);
    if (size < LoadoutBlockSlots(count, recordSlots))
        return false;
    std::vector<Loadout> parsed;
    for (std::size_t index = 0; index < count; ++index)
    {
        std::size_t const record = 1 + index * recordSlots;
        Loadout loadout;
        if (!ReadStringSlots(slots, size, record, LoadoutUuidBytes, loadout.Uuid))
            return false;
        std::size_t const name = record + 1 + LoadoutUuidSlots;
        if (!ReadStringSlots(slots, size, name, LoadoutNameBytes, loadout.Name))
            return false;
        loadout.SortOrder = slots[name + 1 + LoadoutNameSlots];
        loadout.SpecId = slots[name + 2 + LoadoutNameSlots];
        if (recordSlots == LoadoutRecordSlotsV4)
        {
            std::size_t const entries = name + 3 + LoadoutNameSlots;
            std::size_t const stored = ClampLoadoutEntries(slots[entries]);
            for (std::size_t item = 0; item < stored; ++item)
                loadout.Entries.push_back({ slots[entries + 1 + item * 3], slots[entries + 2 + item * 3], 0,
                                            slots[entries + 3 + item * 3] != 0, 0 });
        }
        if (!loadout.Uuid.empty())
            parsed.push_back(std::move(loadout));
    }
    std::string active;
    if (!ReadStringSlots(slots, size, 1 + count * recordSlots, LoadoutUuidBytes, active))
        return false;
    loadouts = std::move(parsed);
    activeUuid = std::move(active);
    return true;
}

std::vector<KnownEntry> SpecializationSwitch(std::uint8_t classId, HasSpell const& hasSpell, std::uint32_t specId)
{
    (void)hasSpell;
    return DefaultEntries(classId, specId);
}
}
