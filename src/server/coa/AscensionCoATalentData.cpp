/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionCoATalentData.h"
#include "ClientDBC.h"
#include "DBCStores.h"
#include "Log.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace AscensionCompatData
{
std::vector<CoATalentEntry> CoATalentEntries;
std::vector<CoASelectableFreeEntry> CoASelectableFreeEntries;
std::vector<CoAAutomaticDependency> CoAAutomaticDependencies;
std::vector<CoASpecialization> CoASpecializations;
std::vector<CoATalentBudget> CoATalentBudgets;

namespace
{
constexpr uint32 SHARED_CLASS_TAB_ID = 87;
constexpr uint32 ADVANCEMENT_REQUIRED_COUNT = 3;
constexpr uint32 ADVANCEMENT_RANK_COUNT = 5;

constexpr std::array<uint32, 2> IDENTITY_PASSIVES_KEEPING_AUTHORED_GATES = { 4037, 4041 };

enum AdvancementDwordField : uint32
{
    ADVANCEMENT_ID           = 0,
    ADVANCEMENT_TYPE         = 1,
    ADVANCEMENT_REQUIRED     = 2,
    ADVANCEMENT_SPELLS       = 5,
    ADVANCEMENT_AE_COST      = 14,
    ADVANCEMENT_TE_COST      = 15,
    ADVANCEMENT_LEVEL        = 26,
    ADVANCEMENT_GROUP        = 29,
    ADVANCEMENT_CLASS_TYPE   = 32,
    ADVANCEMENT_TAB          = 33,
    ADVANCEMENT_GATE_GLOBAL_AE = 34,
    ADVANCEMENT_GATE_GLOBAL_TE = 35,
    ADVANCEMENT_GATE_CLASS_AE  = 36,
    ADVANCEMENT_GATE_CLASS_TE  = 37,
    ADVANCEMENT_GATE_TAB_AE    = 38,
    ADVANCEMENT_GATE_TAB_TE    = 39,
    ADVANCEMENT_POINTS         = 40,
    ADVANCEMENT_POINTS_GATE    = 41,
    ADVANCEMENT_NAME         = 47,
    ADVANCEMENT_TREE_ROW     = 101,
};

constexpr uint32 ADVANCEMENT_FLAGS_BYTE = 479;
constexpr uint32 ADVANCEMENT_FLAG_FREE_TO_UNLEARN = 0x2000;
constexpr uint32 ADVANCEMENT_FLAG_FREE_TO_LEARN = 0x100000;
constexpr uint32 ADVANCEMENT_FLAG_GRANTED = ADVANCEMENT_FLAG_FREE_TO_UNLEARN | ADVANCEMENT_FLAG_FREE_TO_LEARN;

std::unordered_map<uint32, uint32> rowFlags;

enum ChrSpecsDwordField : uint32
{
    CHR_SPECS_ID              = 0,
    CHR_SPECS_SIGNATURE_SPELL = 24,
    CHR_SPECS_IDENTITY_ENTRY  = 28,
};

enum EssenceDwordField : uint32
{
    ESSENCE_LEVEL = 1,
    ESSENCE_KEY   = 2,
    ESSENCE_FLAGS = 3,
    ESSENCE_AE    = 7,
    ESSENCE_TE    = 8,
};

bool Contains(auto const& values, uint32 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string JoinEntryIds(std::vector<uint32> const& entryIds)
{
    std::string listed;
    for (uint32 const entryId : entryIds)
    {
        if (!listed.empty())
            listed += ", ";
        listed += std::to_string(entryId);
    }
    return listed;
}

std::string Upper(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return result;
}

struct Node
{
    CoATalentEntry Entry;
    std::vector<uint32> Required;
    uint32 Group;
    uint32 TreeRow;
    bool ClassTab;
};

bool IsFreeChoice(Node const& node)
{
    return !node.Entry.AECost && !node.Entry.TECost && node.Group;
}

struct AdvancementClassType
{
    uint32 ClassId;
    bool IsCustomClass;
};

uint32 AdvancementFlags(ClientDBC::Record const& record)
{
    uint32 flags = 0;
    for (uint32 byte = 0; byte < 4; ++byte)
        flags |= uint32(record.GetUInt8(ADVANCEMENT_FLAGS_BYTE + byte)) << (8 * byte);
    return flags;
}
}

bool GetCoATalentBudget(std::uint8_t classId, std::uint8_t level, std::uint32_t& ae, std::uint32_t& te)
{
    CoATalentBudget const* row = nullptr;
    for (CoATalentBudget const& budget : CoATalentBudgets)
    {
        if (budget.ClassId != classId || budget.Level > level)
            continue;
        if (!row || budget.Level > row->Level)
            row = &budget;
    }
    if (!row)
        return false;

    ae = row->AE;
    te = row->TE;
    return true;
}

bool IsCoAFreeToUnlearnRow(std::uint32_t entryId)
{
    auto const row = rowFlags.find(entryId);
    return row != rowFlags.end() && (row->second & ADVANCEMENT_FLAG_FREE_TO_UNLEARN) != 0;
}

bool IsCoAGrantedRow(std::uint32_t entryId)
{
    auto const row = rowFlags.find(entryId);
    return row != rowFlags.end() && (row->second & ADVANCEMENT_FLAG_GRANTED) != 0;
}

bool LoadCoATalentData()
{
    CoATalentEntries.clear();
    CoASelectableFreeEntries.clear();
    CoAAutomaticDependencies.clear();
    CoASpecializations.clear();
    CoATalentBudgets.clear();
    rowFlags.clear();

    ClientDBC classes, classTypes, tabTypes, specs, advancement, essence;
    if (!classes.Load(GetClientDBCPath("ChrClasses.dbc"), 56) ||
        !classTypes.Load(GetClientDBCPath("CharacterAdvancementClassTypes.dbc"), 5) ||
        !tabTypes.Load(GetClientDBCPath("CharacterAdvancementTabTypes.dbc"), 2) ||
        !specs.Load(GetClientDBCPath("ChrSpecs.dbc"), 29) ||
        !advancement.Load(GetClientDBCPath("CharacterAdvancement.dbc"),
                          (ADVANCEMENT_FLAGS_BYTE + 4 + 3) / 4) ||
        !essence.Load(GetClientDBCPath("CharacterAdvancementEssence.dbc"), ESSENCE_TE + 1))
        return false;

    for (uint32 row = 0; row < essence.GetRecordCount(); ++row)
    {
        ClientDBC::Record record = essence.GetRecord(row);
        uint32 const key = record.GetUInt32(ESSENCE_KEY);
        uint32 const level = record.GetUInt32(ESSENCE_LEVEL);
        if (key < 12 || key > 32 || !level || level > 255)
            continue;
        if (record.GetUInt32(ESSENCE_FLAGS) || record.GetUInt32(ESSENCE_FLAGS + 1) ||
            record.GetUInt32(ESSENCE_FLAGS + 2) || record.GetUInt32(ESSENCE_FLAGS + 3))
            continue;

        CoATalentBudgets.push_back({ uint8(key), uint8(level),
            uint8(std::min<uint32>(record.GetUInt32(ESSENCE_AE), 255)),
            uint8(std::min<uint32>(record.GetUInt32(ESSENCE_TE), 255)) });
    }
    std::sort(CoATalentBudgets.begin(), CoATalentBudgets.end(),
        [](CoATalentBudget const& left, CoATalentBudget const& right)
        {
            return std::tie(left.ClassId, left.Level) < std::tie(right.ClassId, right.Level);
        });

    std::unordered_map<uint32, std::string> classTokens;
    for (uint32 row = 0; row < classes.GetRecordCount(); ++row)
        classTokens[classes.GetRecord(row).GetUInt32(0)] = std::string(classes.GetRecord(row).GetString(55));

    std::unordered_map<uint32, AdvancementClassType> classTypeById;
    for (uint32 row = 0; row < classTypes.GetRecordCount(); ++row)
    {
        ClientDBC::Record record = classTypes.GetRecord(row);
        classTypeById[record.GetUInt32(0)] = { record.GetUInt32(2), record.GetUInt32(4) != 0 };
    }

    std::unordered_map<uint32, std::string> tabTokens;
    for (uint32 row = 0; row < tabTypes.GetRecordCount(); ++row)
        tabTokens[tabTypes.GetRecord(row).GetUInt32(0)] = Upper(tabTypes.GetRecord(row).GetString(1));

    std::unordered_map<uint32, uint32> classOfEntry;
    for (uint32 row = 0; row < advancement.GetRecordCount(); ++row)
    {
        ClientDBC::Record record = advancement.GetRecord(row);
        uint32 const entryId = record.GetUInt32(ADVANCEMENT_ID);
        rowFlags[entryId] = AdvancementFlags(record);
        auto classType = classTypeById.find(record.GetUInt32(ADVANCEMENT_CLASS_TYPE));
        if (classType != classTypeById.end())
            classOfEntry[entryId] = classType->second.ClassId;
    }

    std::map<std::pair<std::string, std::string>, uint32> specByClassAndTab;
    std::unordered_map<uint32, uint32> identitySpecByEntry;
    std::vector<std::tuple<uint32, uint32, uint32>> specIdentities;
    for (uint32 row = 0; row < specs.GetRecordCount(); ++row)
    {
        ClientDBC::Record record = specs.GetRecord(row);
        uint32 const specId = record.GetUInt32(CHR_SPECS_ID);
        uint32 const identity = record.GetUInt32(CHR_SPECS_IDENTITY_ENTRY);

        std::string classToken(record.GetString(1));
        if (classToken.empty())
        {
            auto entryClass = classOfEntry.find(identity);
            if (entryClass != classOfEntry.end())
                classToken = classTokens[entryClass->second];
        }

        specByClassAndTab[{ classToken, std::string(record.GetString(2)) }] = specId;
        if (identity)
        {
            identitySpecByEntry[identity] = specId;
            specIdentities.emplace_back(specId, identity, record.GetUInt32(CHR_SPECS_SIGNATURE_SPELL));
        }
    }

    std::vector<Node> nodes;
    uint32 unmappedTabRows = 0;
    for (uint32 row = 0; row < advancement.GetRecordCount(); ++row)
    {
        ClientDBC::Record record = advancement.GetRecord(row);
        uint32 const entryId = record.GetUInt32(ADVANCEMENT_ID);
        auto classType = classTypeById.find(record.GetUInt32(ADVANCEMENT_CLASS_TYPE));
        if (classType == classTypeById.end() || !classType->second.IsCustomClass ||
            classType->second.ClassId < 12 || classType->second.ClassId > 32)
            continue;

        uint32 const classId = classType->second.ClassId;
        uint32 const tab = record.GetUInt32(ADVANCEMENT_TAB);
        uint32 specId = 0;
        if (tab != SHARED_CLASS_TAB_ID)
        {
            auto spec = specByClassAndTab.find({ classTokens[classId], tabTokens[tab] });
            if (spec == specByClassAndTab.end())
            {
                if (++unmappedTabRows <= 10)
                    LOG_INFO("coa",
                        "CoA row {} ({}) is on tab {} of class {}, which names no archetype; skipped",
                        entryId, record.GetString(ADVANCEMENT_NAME), tab, uint32(classId));
                continue;
            }
            specId = spec->second;
        }

        Node node{};
        node.Entry.EntryId = entryId;
        node.Entry.ClassId = uint8(classId);
        node.Entry.SpecId = uint16(specId);
        node.Entry.AECost = uint8(record.GetUInt32(ADVANCEMENT_AE_COST));
        node.Entry.TECost = uint8(record.GetUInt32(ADVANCEMENT_TE_COST));
        node.Entry.RequiredLevel = uint8(record.GetUInt32(ADVANCEMENT_LEVEL));
        std::string_view const typeToken = record.GetString(ADVANCEMENT_TYPE);
        node.Entry.Type = typeToken == "Ability"         ? COA_ENTRY_ABILITY
                        : typeToken == "Talent"          ? COA_ENTRY_TALENT
                        : typeToken == "Trait"           ? COA_ENTRY_TRAIT
                        : typeToken == "TalentAbility"   ? COA_ENTRY_TALENT_ABILITY
                                                         : COA_ENTRY_NONE;
        node.Entry.TabId = uint8(tab);
        node.Entry.GateAE = { uint16(record.GetUInt32(ADVANCEMENT_GATE_GLOBAL_AE)),
                              uint16(record.GetUInt32(ADVANCEMENT_GATE_CLASS_AE)),
                              uint16(record.GetUInt32(ADVANCEMENT_GATE_TAB_AE)) };
        node.Entry.GateTE = { uint16(record.GetUInt32(ADVANCEMENT_GATE_GLOBAL_TE)),
                              uint16(record.GetUInt32(ADVANCEMENT_GATE_CLASS_TE)),
                              uint16(record.GetUInt32(ADVANCEMENT_GATE_TAB_TE)) };
        node.Entry.PointsGate = uint16(record.GetUInt32(ADVANCEMENT_POINTS_GATE));
        node.Entry.Points = uint16(record.GetUInt32(ADVANCEMENT_POINTS));
        node.Group = record.GetUInt32(ADVANCEMENT_GROUP);
        node.TreeRow = record.GetUInt32(ADVANCEMENT_TREE_ROW);
        node.ClassTab = tab == SHARED_CLASS_TAB_ID;

        bool tooManyRanks = false;
        for (uint32 field = ADVANCEMENT_SPELLS; field < ADVANCEMENT_SPELLS + ADVANCEMENT_RANK_COUNT; ++field)
        {
            uint32 const spellId = record.GetUInt32(field);
            if (!spellId)
                continue;
            if (node.Entry.SpellCount == node.Entry.SpellIds.size())
            {
                tooManyRanks = true;
                break;
            }
            node.Entry.SpellIds[node.Entry.SpellCount++] = spellId;
        }

        if (tooManyRanks)
        {
            LOG_ERROR("coa", "Skipped CoA talent entry {} with more than 3 ranks", entryId);
            continue;
        }

        for (uint32 field = ADVANCEMENT_REQUIRED; field < ADVANCEMENT_REQUIRED + ADVANCEMENT_REQUIRED_COUNT; ++field)
            if (uint32 requiredId = record.GetUInt32(field))
                node.Required.push_back(requiredId);

        auto identity = identitySpecByEntry.find(entryId);
        if (identity != identitySpecByEntry.end() && identity->second == specId &&
            !Contains(IDENTITY_PASSIVES_KEEPING_AUTHORED_GATES, entryId))
        {
            node.Entry.RequiredLevel = 10;
            node.Required.clear();
        }

        nodes.push_back(std::move(node));
    }

    std::sort(nodes.begin(), nodes.end(), [](Node const& left, Node const& right)
    {
        return left.Entry.EntryId < right.Entry.EntryId;
    });

    std::unordered_map<uint32, Node const*> nodeById;
    for (Node const& node : nodes)
        nodeById[node.Entry.EntryId] = &node;

    for (Node const& node : nodes)
    {
        CoATalentEntries.push_back(node.Entry);
        if (IsFreeChoice(node))
            CoASelectableFreeEntries.push_back({ node.Entry.EntryId, node.Group });

        if (node.Entry.AECost || node.Entry.TECost || node.Required.empty())
            continue;

        std::vector<uint32> required;
        for (uint32 requiredId : node.Required)
        {
            auto requiredNode = nodeById.find(requiredId);
            bool const paidClassNode = requiredNode != nodeById.end() && requiredNode->second->ClassTab &&
                (requiredNode->second->Entry.AECost || requiredNode->second->Entry.TECost);
            if (!(node.Entry.SpecId && !IsFreeChoice(node) && paidClassNode))
                required.push_back(requiredId);
        }

        if (required.empty())
            continue;

        if (required.size() > 2)
        {
            LOG_ERROR("coa", "Skipped CoA talent entry {} dependencies: {} required entries",
                node.Entry.EntryId, required.size());
            continue;
        }

        CoAAutomaticDependency dependency{ node.Entry.EntryId, {} };
        std::copy(required.begin(), required.end(), dependency.RequiredEntryIds.begin());
        CoAAutomaticDependencies.push_back(dependency);
    }

    for (auto const& [specId, identityId, signatureSpellId] : specIdentities)
    {
        auto identity = nodeById.find(identityId);
        if (identity == nodeById.end() || identity->second->Entry.SpecId != specId)
            continue;

        uint32 signatureId = 0;
        for (Node const& node : nodes)
            if (signatureSpellId && node.Entry.ClassId == identity->second->Entry.ClassId &&
                Contains(node.Entry.SpellIds, signatureSpellId))
            {
                signatureId = node.Entry.EntryId;
                break;
            }

        auto belongsToArchetype = [&nodeById, specId](uint32 entryId)
        {
            auto node = nodeById.find(entryId);
            return node != nodeById.end() && node->second->Entry.SpecId == specId;
        };

        if (signatureId && !belongsToArchetype(signatureId))
        {
            auto node = nodeById.find(signatureId);
            uint32 const otherSpec = node == nodeById.end() ? 0 : uint32(node->second->Entry.SpecId);
            if (otherSpec)
            {
                LOG_ERROR("coa",
                          "CoA archetype {} signature spell {} resolves to row {} of archetype {}, not to a class-tab "
                          "row; the archetype keeps no class-tree default rather than another archetype's talent",
                          uint32(specId), signatureSpellId, signatureId, otherSpec);
                signatureId = 0;
            }
        }

        CoASpecialization spec{ uint16(specId), identity->second->Entry.ClassId, identityId, signatureId };

        std::vector<uint32> billedOpeners;
        for (Node const& node : nodes)
            if (node.Entry.SpecId == specId && node.Entry.EntryId != identityId && !node.TreeRow)
            {
                spec.OpeningRowEntryIds.push_back(node.Entry.EntryId);
                if (!IsCoAFreeToUnlearnRow(node.Entry.EntryId))
                    billedOpeners.push_back(node.Entry.EntryId);
            }

        if (!spec.OpeningRowEntryIds.empty())
            LOG_INFO("coa",
                "CoA archetype {} opens its own tree on row(s) [{}]; the default state is the "
                "signature {}, the identity {} and the opening rank(s)",
                uint32(specId), JoinEntryIds(spec.OpeningRowEntryIds),
                spec.SignatureEntryId, spec.IdentityEntryId);
        else
            LOG_ERROR("coa", "CoA archetype {} derives no opening row; its right tree opens on nothing",
                uint32(specId));
        if (!billedOpeners.empty())
            LOG_INFO("coa",
                "CoA archetype {} opens on row(s) [{}] that the data does not mark free to unlearn; "
                "entering grants them and leaving prices them",
                uint32(specId), JoinEntryIds(billedOpeners));
        if (spec.OpeningRowEntryIds.size() > 2)
            LOG_ERROR("coa",
                      "CoA archetype {} derives {} opening rows [{}]; the opening row is one, two at most, "
                      "so one of these is not an opening row",
                      uint32(specId), spec.OpeningRowEntryIds.size(),
                      JoinEntryIds(spec.OpeningRowEntryIds));

        CoASpecializations.push_back(std::move(spec));
    }

    std::map<uint8, uint32> archetypesByClass;
    std::map<uint8, uint32> rowsByClass;
    for (CoASpecialization const& spec : CoASpecializations)
        ++archetypesByClass[spec.ClassId];
    for (CoATalentEntry const& entry : CoATalentEntries)
        if (entry.SpecId)
            ++rowsByClass[entry.ClassId];

    std::string archetypeSummary;
    for (uint8 classId = 12; classId <= 32; ++classId)
    {
        auto archetypes = archetypesByClass.find(classId);
        uint32 const count = archetypes == archetypesByClass.end() ? 0 : archetypes->second;
        if (!count)
            LOG_ERROR("coa",
                "CoA class {} derived no archetypes; its archetype trees are invisible to the server",
                uint32(classId));
        else if (!rowsByClass.count(classId))
            LOG_ERROR("coa",
                "CoA class {} has {} archetype(s) but no archetype rows; its trees would be empty",
                uint32(classId), count);

        if (!archetypeSummary.empty())
            archetypeSummary += ", ";
        archetypeSummary += std::to_string(uint32(classId)) + ":" + std::to_string(count);
    }

    LOG_INFO("coa", "Loaded {} CoA talent entries ({} selectable free, {} automatic dependencies, "
        "{} specializations, {} budget rows)",
        CoATalentEntries.size(), CoASelectableFreeEntries.size(), CoAAutomaticDependencies.size(),
        CoASpecializations.size(), CoATalentBudgets.size());
    LOG_INFO("coa", "CoA archetypes per class (12..32): {}", archetypeSummary);
    if (unmappedTabRows)
        LOG_INFO("coa",
            "CoA rows on a tab that names no archetype: {} (listed above; they belong to no tree)",
            unmappedTabRows);
    return true;
}
}
