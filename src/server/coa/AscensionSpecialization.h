#ifndef ASCENSION_SPECIALIZATION_H
#define ASCENSION_SPECIALIZATION_H

#include "AscensionCoATalentState.h"
#include "Define.h"

#include <functional>
#include <string>
#include <vector>

class Player;

uint32 GetAscensionActiveSpecialization(Player const* player);

struct AscensionActiveSpecSlot
{
    uint32 Slot = 0;
    uint32 Count = 1;
};

using AscensionActiveSpecSlotProvider = std::function<AscensionActiveSpecSlot(Player* player)>;

void SetAscensionActiveSpecSlotProvider(AscensionActiveSpecSlotProvider provider);

std::vector<AscensionCoATalentState::KnownEntry> GetAscensionTalentState(Player const* player);

bool ApplyAscensionTalentBuild(Player* player, uint32 specializationId,
                               std::vector<AscensionCoATalentState::KnownEntry> const& entries,
                               std::string* refusal);

void PushAscensionAdvancementState(Player* player);
void PushAscensionActiveSpec(Player* player);

bool SwitchAscensionSpecialization(Player* player, uint32 specializationId);

using AscensionSpecializationSwitchGuard =
    std::function<std::string(Player* player, uint32 activeSpecializationId, uint32 requestedSpecializationId)>;

void AddAscensionSpecializationSwitchGuard(AscensionSpecializationSwitchGuard guard);

std::string AscensionSpecializationSwitchRefusal(Player* player, uint32 activeSpecializationId,
    uint32 requestedSpecializationId);

uint32 ForgetAscensionClassTalents(Player* player);

void ClearAscensionTalentState(Player* player);

uint32 GetAscensionTalentRank(Player const* player, uint32 entryId);

bool SetAscensionTalentRank(Player* player, uint32 entryId, uint32 rank);

bool IsAscensionTalentStateApplying(Player const* player);
bool CanLearnAscensionTalentSpell(Player const* player, uint32 spellId);

bool IsAscensionCustomClassId(uint8 classId);

struct AscensionClassAbility
{
    uint32 SpellId;
    uint32 FirstSpellId;
    uint16 SpecId;
    uint8 RequiredLevel;
};

std::vector<AscensionClassAbility> GetAscensionClassAbilities(uint8 classId);

#endif
