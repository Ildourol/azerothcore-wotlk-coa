#include "CoACampingMapping.h"
#include "CoACampingRules.h"
#include "Chat.h"
#include "CharacterDatabase.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace
{
using namespace CoACamping;

struct CampingSettings
{
    bool Enabled = false;
    std::set<uint32> AllowedMaps;
    std::set<uint32> AllowedAreas;
    uint32 LifetimeSeconds = 900;
    uint32 RestSeconds = 60;
    uint32 BenefitSeconds = 3600;
    uint32 ContributionCooldownSeconds = 3600;
    uint32 ScanMilliseconds = 1000;
    uint32 Capacity = 3;
    float BenefitRadius = 20.0f;
    float ContributionDistance = 5.0f;
    float MinimumSpacing = 40.0f;
};

CampingSettings Settings;

class ObjectCollisionScope
{
public:
    explicit ObjectCollisionScope(GameObject* object) : Object(object),
        Enabled(object && object->m_model && object->m_model->isEnabled())
    {
        if (Object)
            Object->EnableCollision(false);
    }

    ~ObjectCollisionScope()
    {
        if (Object)
            Object->EnableCollision(Enabled);
    }

private:
    GameObject* const Object;
    bool const Enabled;
};

struct Camp
{
    uint64 Id;
    ObjectGuid Fire;
    ObjectGuid Owner;
    ObjectGuid Controller;
    ObjectGuid RewardCaster;
    uint32 Phase;
    uint64 ExpiresAt;
    bool LifetimeSynchronized = false;
    std::vector<ObjectGuid> Attachments;
    std::set<ObjectGuid> Contributors;
};

struct ContributionRequest
{
    ObjectGuid Player;
    ObjectGuid Controller;
    uint64 CampId;
};

struct MapState : DataMap::Base
{
    uint64 NextId = 1;
    uint64 NextScan = 0;
    std::map<ObjectGuid, Camp> Camps;
    std::mutex RequestsMutex;
    std::vector<ContributionRequest> Requests;
    std::vector<ObjectGuid> Removals;
};

struct PlayerState : DataMap::Base
{
    ObjectGuid ActiveFire;
    ObjectGuid MenuController;
    uint64 MenuCampId = 0;
    RestProgress Rest;
    Position RestPosition;
    uint32 RestPhase = 0;
};

MapState* FindState(Map* map)
{
    return map ? map->CustomData.Get<MapState>(MapStateKey) : nullptr;
}

PlayerState& StateFor(Player* player)
{
    return *player->CustomData.GetDefault<PlayerState>(PlayerStateKey);
}

uint64 NowMs()
{
    return GameTime::GetGameTimeMS().count();
}

uint64 NowSeconds()
{
    return GameTime::GetCalendarTime().count();
}

Player* FindPlayerOnMap(Map* map, ObjectGuid guid)
{
    Player* player = ObjectAccessor::FindPlayer(guid);
    return player && player->IsInWorld() && player->FindMap() == map ? player : nullptr;
}

void Message(Player* player, char const* text)
{
    ChatHandler(player->GetSession()).SendSysMessage(text);
}

uint64 ReadSettingPair(Player const* player, uint32 index)
{
    PlayerSettingVector const* values = player->FindPlayerSettings(CooldownSettings);
    if (!values || values->size() <= index + 1)
        return 0;
    return uint64((*values)[index].value) | (uint64((*values)[index + 1].value) << 32);
}

void WriteSettingPair(Player* player, uint32 index, uint64 value)
{
    player->UpdatePlayerSetting(CooldownSettings, index, uint32(value));
    player->UpdatePlayerSetting(CooldownSettings, index + 1, uint32(value >> 32));
}

bool Available(Player const* player)
{
    return player->IsInWorld() && player->IsAlive() && !player->IsInCombat() && !player->IsFlying() &&
        !player->IsInFlight() && !player->GetTransport() && !player->GetVehicle() && !player->IsUnderWater();
}

bool AllowedLocation(Player const* player)
{
    Map* map = player->FindMap();
    return map && !map->Instanceable() && Settings.AllowedMaps.count(map->GetId()) &&
        (Settings.AllowedAreas.empty() || Settings.AllowedAreas.count(player->GetAreaId())) && player->IsOutdoors();
}

bool VisibleObject(Player const* player, GameObject* object)
{
    if (!object || !object->IsInWorld() || !player->IsInMap(object))
        return false;
    ObjectCollisionScope scope(object);
    return player->CanSeeOrDetect(object) &&
        player->IsWithinLOS(object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
}

bool Accessible(Player const* player, Camp const& camp, GameObject* fire)
{
    if (!fire || !fire->IsInWorld() || player->FindMap() != fire->FindMap() ||
        player->GetPhaseMask() != camp.Phase || fire->GetPhaseMask() != camp.Phase)
        return false;
    return VisibleObject(player, fire);
}

Camp* FindCamp(MapState* state, ObjectGuid controller, uint64 id = 0)
{
    if (!state)
        return nullptr;
    for (auto& [guid, camp] : state->Camps)
        if (camp.Controller == controller && (!id || camp.Id == id))
            return &camp;
    return nullptr;
}

void RemoveCamp(Map* map, ObjectGuid fireGuid, bool removeFire)
{
    MapState* state = FindState(map);
    if (!state)
        return;
    auto const found = state->Camps.find(fireGuid);
    if (found == state->Camps.end())
        return;
    Camp camp = std::move(found->second);
    state->Camps.erase(found);

    for (auto const& reference : map->GetPlayers())
    {
        Player* player = reference.GetSource();
        PlayerState* playerState = player->CustomData.Get<PlayerState>(PlayerStateKey);
        if (!playerState)
            continue;
        if (playerState->ActiveFire == fireGuid)
            playerState->ActiveFire.Clear();
        if (playerState->Rest.CampId == camp.Id)
            playerState->Rest.Reset();
        if (playerState->MenuController == camp.Controller)
        {
            playerState->MenuController.Clear();
            playerState->MenuCampId = 0;
        }
    }

    camp.Attachments.push_back(camp.Controller);
    for (ObjectGuid guid : camp.Attachments)
        if (GameObject* object = map->GetGameObject(guid))
            object->Delete();
    if (Creature* creature = map->GetCreature(camp.RewardCaster))
        creature->DespawnOrUnsummon();
    if (GameObject* fire = map->GetGameObject(fireGuid))
    {
        if (GameObject* trap = fire->GetLinkedTrap())
            trap->Delete();
        if (removeFire)
            fire->Delete();
    }
}

bool HasNearbyCamp(Player const* player, WorldObject const* position)
{
    MapState* state = FindState(player->FindMap());
    if (!state)
        return false;
    for (auto const& [guid, camp] : state->Camps)
        if (camp.ExpiresAt > NowMs() && camp.Phase == player->GetPhaseMask())
            if (GameObject* fire = player->FindMap()->GetGameObject(guid))
                if (position->IsWithinDistInMap(fire, Settings.MinimumSpacing))
                    return true;
    return false;
}

bool PlacementAllowed(Player* player, WorldObject* position)
{
    if (!Available(player) || !AllowedLocation(player) || StateFor(player).ActiveFire ||
        HasNearbyCamp(player, position))
        return false;
    ObjectCollisionScope scope(position->ToGameObject());
    Map* map = player->FindMap();
    float const height = map->GetHeight(player->GetPhaseMask(), position->GetPositionX(),
        position->GetPositionY(), position->GetPositionZ() + 2.0f);
    return std::isfinite(height) && std::abs(height - position->GetPositionZ()) <= 3.0f &&
        player->IsWithinLOS(position->GetPositionX(), position->GetPositionY(), position->GetPositionZ());
}

GameObject* SummonProp(GameObject* fire, Player* owner, uint32 entry, float offset, uint32 seconds)
{
    ObjectCollisionScope scope(fire);
    Position position = fire->GetNearPosition(offset, float(M_PI) / 2.0f);
    float const height = fire->GetMap()->GetHeight(fire->GetPhaseMask(), position.GetPositionX(),
        position.GetPositionY(), position.GetPositionZ() + 2.0f);
    if (!std::isfinite(height) || std::abs(height - position.GetPositionZ()) > 3.0f)
        return nullptr;
    position.m_positionZ = height;
    float const eyeHeight = owner->GetCollisionHeight();
    if (!fire->GetMap()->isInLineOfSight(fire->GetPositionX(), fire->GetPositionY(),
        fire->GetPositionZ() + eyeHeight, position.GetPositionX(), position.GetPositionY(), height + eyeHeight,
        fire->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
        return nullptr;
    GameObject* prop = fire->SummonGameObject(entry, position.GetPositionX(), position.GetPositionY(), height,
        position.GetOrientation(), 0.0f, 0.0f, std::sin(position.GetOrientation() / 2.0f),
        std::cos(position.GetOrientation() / 2.0f), seconds);
    if (prop)
    {
        prop->SetOwnerGUID(owner->GetGUID());
        prop->SetSpellId(0);
    }
    return prop;
}

void CreateCamp(GameObject* fire)
{
    Player* owner = FindPlayerOnMap(fire->GetMap(), fire->GetOwnerGUID());
    if (!owner || !PlacementAllowed(owner, fire))
        return;
    MapState* state = FindState(fire->GetMap());
    if (!state || state->Camps.count(fire->GetGUID()))
        return;
    GameObject* controller = SummonProp(fire, owner, ControllerEntry, 2.0f, Settings.LifetimeSeconds);
    if (!controller)
    {
        Message(owner, "The campsite supplies could not be placed. This remains an ordinary campfire.");
        return;
    }
    Creature* rewardCaster = fire->SummonTrigger(fire->GetPositionX(), fire->GetPositionY(),
        fire->GetPositionZ(), fire->GetOrientation(), Settings.LifetimeSeconds * IN_MILLISECONDS);
    if (!rewardCaster)
    {
        controller->Delete();
        Message(owner, "The campsite could not be created. This remains an ordinary campfire.");
        return;
    }
    rewardCaster->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
    rewardCaster->SetReactState(REACT_PASSIVE);
    fire->SetRespawnTime(Settings.LifetimeSeconds);
    ObjectGuid const fireGuid = fire->GetGUID();
    state->Camps.emplace(fireGuid, Camp{state->NextId++, fireGuid, owner->GetGUID(), controller->GetGUID(),
        rewardCaster->GetGUID(), fire->GetPhaseMask(), NowMs() + uint64(Settings.LifetimeSeconds) * IN_MILLISECONDS});
    StateFor(owner).ActiveFire = fireGuid;
    ChatHandler(owner->GetSession()).PSendSysMessage(
        "Campsite ready for {} seconds. Use the campsite supplies to contribute an incense candle.",
        Settings.LifetimeSeconds);
}

char const* FailureMessage(ContributionFailure failure)
{
    switch (failure)
    {
        case ContributionFailure::Expired: return "This campsite has expired.";
        case ContributionFailure::Inaccessible: return "This campsite is no longer accessible.";
        case ContributionFailure::TooFar: return "Move closer to the campsite supplies.";
        case ContributionFailure::Busy: return "Contribute while alive, out of combat, and on the ground.";
        case ContributionFailure::Skill: return "An incense candle requires Herbalism 20.";
        case ContributionFailure::Materials: return "An incense candle requires one Peacebloom and one Silverleaf.";
        case ContributionFailure::Cooldown: return "Your shared camping contribution cooldown is still active.";
        case ContributionFailure::AlreadyContributed: return "You have already contributed to this campsite.";
        case ContributionFailure::Full: return "This campsite has no attachment slots remaining.";
        case ContributionFailure::FeaturePresent: return "This campsite already has an incense candle.";
        default: return "The contribution could not be placed.";
    }
}

void Contribute(Map* map, ContributionRequest const& request)
{
    Player* player = FindPlayerOnMap(map, request.Player);
    if (!player)
        return;
    Camp* camp = FindCamp(FindState(map), request.Controller, request.CampId);
    if (!camp)
    {
        Message(player, FailureMessage(ContributionFailure::Expired));
        return;
    }
    GameObject* fire = map->GetGameObject(camp->Fire);
    GameObject* controller = map->GetGameObject(camp->Controller);
    ObjectCollisionScope fireScope(fire);
    ContributionCheck check;
    check.Expired = NowMs() >= camp->ExpiresAt;
    check.Accessible = Accessible(player, *camp, fire) && VisibleObject(player, controller);
    check.InRange = controller && player->IsWithinDistInMap(controller, Settings.ContributionDistance);
    check.Available = Available(player);
    check.HasSkill = player->GetBaseSkillValue(Herbalism) >= RequiredHerbalism;
    check.HasMaterials = std::all_of(CandleMaterials.begin(), CandleMaterials.end(), [player](Material material)
    {
        return player->HasItemCount(material.Item, material.Count, false);
    });
    check.OnCooldown = CooldownActive(ReadSettingPair(player, 0), NowSeconds());
    check.AlreadyContributed = camp->Contributors.count(player->GetGUID());
    check.FeaturePresent = !camp->Attachments.empty();
    check.UsedSlots = uint32(camp->Attachments.size());
    check.Capacity = Settings.Capacity;
    ContributionFailure const failure = CheckContribution(check);
    if (failure != ContributionFailure::None)
    {
        Message(player, FailureMessage(failure));
        return;
    }
    uint32 const seconds = uint32((camp->ExpiresAt - NowMs() + IN_MILLISECONDS - 1) / IN_MILLISECONDS);
    GameObject* candle = SummonProp(fire, player, CandleEntry, 3.0f, seconds);
    if (!candle)
    {
        Message(player, "The candle could not be placed. Your materials and contribution cooldown are unchanged.");
        return;
    }
    camp->Attachments.push_back(candle->GetGUID());
    camp->Contributors.insert(player->GetGUID());
    for (Material material : CandleMaterials)
        player->DestroyItemCount(material.Item, material.Count, true);
    WriteSettingPair(player, 0, NowSeconds() + Settings.ContributionCooldownSeconds);
    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    player->SaveInventoryAndGoldToDB(transaction);
    transaction->Append(PlayerSettingsStore::PrepareReplaceStatement(player->GetGUID().GetCounter(),
        CooldownSettings, *player->FindPlayerSettings(CooldownSettings)));
    CharacterDatabase.CommitTransaction(transaction);
    ChatHandler(player->GetSession()).PSendSysMessage(
        "Incense candle placed. Sit near the fire for {} seconds to receive +2 Intellect for {} seconds.",
        Settings.RestSeconds, Settings.BenefitSeconds);
}

bool EquivalentIntellect(Aura const* aura)
{
    uint32 root = sSpellMgr->GetFirstSpellInChain(aura->GetId());
    if (!root)
        root = aura->GetId();
    return std::find(IntellectFamilies.begin(), IntellectFamilies.end(), root) != IntellectFamilies.end();
}

Aura* CampingAura(Player* player)
{
    uint64 const caster = ReadSettingPair(player, 2);
    if (!caster)
        return nullptr;
    return player->GetAura(RewardSpell, ObjectGuid(caster));
}

void Reward(Player* player, Camp const& camp)
{
    Aura* campingAura = CampingAura(player);
    for (auto const& [spell, application] : player->GetAppliedAuras())
        if (application->GetBase() != campingAura && EquivalentIntellect(application->GetBase()))
            return;
    if (!campingAura)
    {
        Creature* caster = player->FindMap()->GetCreature(camp.RewardCaster);
        if (!caster)
            return;
        campingAura = caster->AddAura(sSpellMgr->GetSpellInfo(RewardSpell), RewardEffectMask, player);
        if (!campingAura || campingAura->IsRemoved())
            return;
        WriteSettingPair(player, 2, campingAura->GetCasterGUID().GetRawValue());
    }
    if (AuraEffect* effect = campingAura->GetEffect(EFFECT_0))
        effect->ChangeAmount(RewardIntellect);
    campingAura->SetMaxDuration(Settings.BenefitSeconds * IN_MILLISECONDS);
    campingAura->SetDuration(Settings.BenefitSeconds * IN_MILLISECONDS);
    Message(player, "You finish resting at the campsite and gain +2 Intellect.");
}

bool RestEligible(Player const* player)
{
    return Available(player) && player->IsSitState() && !player->isMoving();
}

void UpdateRest(Map* map, MapState& state, uint64 now)
{
    for (auto const& reference : map->GetPlayers())
    {
        Player* player = reference.GetSource();
        PlayerState& playerState = StateFor(player);
        Camp const* nearest = nullptr;
        float nearestDistance = Settings.BenefitRadius;
        if (RestEligible(player))
            for (auto const& [guid, camp] : state.Camps)
            {
                GameObject* fire = map->GetGameObject(guid);
                if (camp.Attachments.empty() || !Accessible(player, camp, fire))
                    continue;
                float const distance = player->GetDistance(fire);
                if (distance <= nearestDistance)
                {
                    nearest = &camp;
                    nearestDistance = distance;
                }
            }
        if (playerState.Rest.Observe(nearest ? nearest->Id : 0, now,
            uint64(Settings.RestSeconds) * IN_MILLISECONDS, nearest != nullptr))
            Reward(player, *nearest);
        playerState.RestPosition = player->GetPosition();
        playerState.RestPhase = player->GetPhaseMask();
    }
}

void UpdateMap(Map* map)
{
    MapState* state = FindState(map);
    if (!state)
        return;
    std::vector<ObjectGuid> departures;
    {
        std::lock_guard<std::mutex> lock(state->RequestsMutex);
        departures.swap(state->Removals);
    }
    for (ObjectGuid guid : departures)
        RemoveCamp(map, guid, true);
    uint64 const now = NowMs();
    std::vector<ObjectGuid> removed;
    for (auto& [guid, camp] : state->Camps)
    {
        GameObject* fire = map->GetGameObject(guid);
        Player* owner = FindPlayerOnMap(map, camp.Owner);
        GameObject* controller = map->GetGameObject(camp.Controller);
        if (!owner || owner->GetPhaseMask() != camp.Phase || !fire || !fire->IsInWorld() ||
            fire->GetPhaseMask() != camp.Phase || !controller || !controller->IsInWorld() ||
            !map->GetCreature(camp.RewardCaster) || now >= camp.ExpiresAt)
        {
            removed.push_back(guid);
            continue;
        }
        if (!camp.LifetimeSynchronized)
        {
            if (GameObject* trap = fire->GetLinkedTrap())
                trap->SetRespawnTime(Settings.LifetimeSeconds);
            camp.LifetimeSynchronized = true;
        }
        std::erase_if(camp.Attachments, [map](ObjectGuid attachment)
        {
            GameObject* object = map->GetGameObject(attachment);
            return !object || !object->IsInWorld();
        });
    }
    for (ObjectGuid guid : removed)
        RemoveCamp(map, guid, true);
    std::vector<ContributionRequest> requests;
    {
        std::lock_guard<std::mutex> lock(state->RequestsMutex);
        requests.swap(state->Requests);
    }
    for (ContributionRequest const& request : requests)
        Contribute(map, request);
    if (now >= state->NextScan)
    {
        state->NextScan = now + Settings.ScanMilliseconds;
        UpdateRest(map, *state, now);
    }
}

void RequireMapping(bool valid, char const* mapping)
{
    if (valid)
        return;
    LOG_FATAL("server.loading", "CoA Camping: invalid or missing {}. Check camping mappings and pending SQL.", mapping);
    ABORT();
}

std::set<uint32> ReadAllowlist(char const* option, char const* fallback)
{
    std::set<uint32> values;
    std::string const setting = sConfigMgr->GetOption<std::string>(option, fallback);
    for (std::string_view value : Acore::Tokenize(setting, ',', false))
    {
        auto const parsed = Acore::StringTo<uint32>(value);
        RequireMapping(parsed.has_value(), option);
        values.insert(*parsed);
    }
    return values;
}

class camping_world : public WorldScript
{
public:
    camping_world() : WorldScript("camping_world", {WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED}) { }

    void OnBeforeWorldInitialized() override
    {
        Settings.Enabled = sConfigMgr->GetOption<bool>("CoACamping.Enable", false);
        if (!Settings.Enabled)
            return;
        Settings.AllowedMaps = ReadAllowlist("CoACamping.AllowedMaps", "0,1,530,571");
        Settings.AllowedAreas = ReadAllowlist("CoACamping.AllowedAreas", "");
        Settings.LifetimeSeconds = sConfigMgr->GetOption<uint32>("CoACamping.LifetimeSeconds", 900);
        Settings.RestSeconds = sConfigMgr->GetOption<uint32>("CoACamping.RestSeconds", 60);
        Settings.BenefitSeconds = sConfigMgr->GetOption<uint32>("CoACamping.BenefitSeconds", 3600);
        Settings.ContributionCooldownSeconds =
            sConfigMgr->GetOption<uint32>("CoACamping.ContributionCooldownSeconds", 3600);
        Settings.ScanMilliseconds = sConfigMgr->GetOption<uint32>("CoACamping.ScanMilliseconds", 1000);
        Settings.Capacity = sConfigMgr->GetOption<uint32>("CoACamping.Capacity", 3);
        Settings.BenefitRadius = sConfigMgr->GetOption<float>("CoACamping.BenefitRadius", 20.0f);
        Settings.ContributionDistance = sConfigMgr->GetOption<float>("CoACamping.ContributionDistance", 5.0f);
        Settings.MinimumSpacing = sConfigMgr->GetOption<float>("CoACamping.MinimumSpacing", 40.0f);
        RequireMapping(!Settings.AllowedMaps.empty(), "allowed maps");
        RequireMapping(Settings.LifetimeSeconds > 0 && Settings.LifetimeSeconds <= 86400, "camp lifetime");
        RequireMapping(Settings.RestSeconds > 0 && Settings.RestSeconds < Settings.LifetimeSeconds, "rest duration");
        RequireMapping(Settings.BenefitSeconds > 0 && Settings.BenefitSeconds <= 86400, "benefit duration");
        RequireMapping(Settings.ContributionCooldownSeconds > 0 && Settings.ContributionCooldownSeconds <= 604800,
            "contribution cooldown");
        RequireMapping(Settings.ScanMilliseconds > 0 && Settings.ScanMilliseconds <= 1000, "scan interval");
        RequireMapping(Settings.Capacity > 0 && Settings.Capacity <= 10, "capacity");
        RequireMapping(std::isfinite(Settings.BenefitRadius) && Settings.BenefitRadius > 0.0f &&
            Settings.BenefitRadius <= 100.0f, "benefit radius");
        RequireMapping(std::isfinite(Settings.ContributionDistance) && Settings.ContributionDistance > 0.0f &&
            Settings.ContributionDistance <= INTERACTION_DISTANCE, "contribution distance");
        RequireMapping(std::isfinite(Settings.MinimumSpacing) &&
            Settings.MinimumSpacing >= Settings.BenefitRadius * 2.0f, "minimum spacing");
        for (uint32 id : Settings.AllowedMaps)
            RequireMapping(sMapStore.LookupEntry(id) && !sMapStore.LookupEntry(id)->Instanceable(), "outdoor map");
        for (uint32 id : Settings.AllowedAreas)
            RequireMapping(sAreaTableStore.LookupEntry(id), "outdoor area");
        GameObjectTemplate const* fire = sObjectMgr->GetGameObjectTemplate(FireEntry);
        GameObjectTemplate const* controller = sObjectMgr->GetGameObjectTemplate(ControllerEntry);
        GameObjectTemplate const* candle = sObjectMgr->GetGameObjectTemplate(CandleEntry);
        RequireMapping(fire && fire->type == GAMEOBJECT_TYPE_SPELL_FOCUS && fire->displayId == FireDisplay,
            "spell-818 campfire template");
        RequireMapping(controller && controller->type == GAMEOBJECT_TYPE_GOOBER &&
            controller->displayId == ControllerDisplay && controller->name == "Campsite Supplies",
            "controller template");
        RequireMapping(candle && candle->type == GAMEOBJECT_TYPE_GENERIC &&
            candle->displayId == CandleDisplay && candle->name == "Incense Candle", "incense candle template");
        for (uint32 id : {FireDisplay, ControllerDisplay, CandleDisplay})
            RequireMapping(sGameObjectDisplayInfoStore.LookupEntry(id), "stock gameobject display");
        RequireMapping(sObjectMgr->GetCreatureTemplate(WORLD_TRIGGER), "reward caster template");
        RequireMapping(sSkillLineStore.LookupEntry(Herbalism), "Herbalism");
        for (Material material : CandleMaterials)
            RequireMapping(sObjectMgr->GetItemTemplate(material.Item), "incense materials");
        SpellInfo const* fireSpell = sSpellMgr->GetSpellInfo(FireSpell);
        SpellInfo const* reward = sSpellMgr->GetSpellInfo(RewardSpell);
        RequireMapping(fireSpell && fireSpell->Effects[EFFECT_0].Effect == SPELL_EFFECT_TRANS_DOOR &&
            fireSpell->Effects[EFFECT_0].MiscValue == int32(FireEntry), "stock campfire spell");
        RequireMapping(reward && reward->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_MOD_STAT &&
            reward->Effects[EFFECT_0].MiscValue == STAT_INTELLECT, "stock Intellect aura");
        LOG_INFO("server.loading", "CoA Camping enabled: {} second camps, {} second rests, +{} Intellect.",
            Settings.LifetimeSeconds, Settings.RestSeconds, RewardIntellect);
    }
};

class camping_objects : public AllGameObjectScript
{
public:
    camping_objects() : AllGameObjectScript("camping_objects") { }

    void OnGameObjectAddWorld(GameObject* object) override
    {
        if (Settings.Enabled && object->GetEntry() == FireEntry && object->GetSpellId() == FireSpell &&
            object->GetOwnerGUID().IsPlayer() && !object->GetSpawnId())
            CreateCamp(object);
    }

    void OnGameObjectRemoveWorld(GameObject* object) override
    {
        if (Settings.Enabled && object->GetEntry() == FireEntry)
            RemoveCamp(object->GetMap(), object->GetGUID(), false);
    }

    bool CanGameObjectGossipHello(Player* player, GameObject* object) override
    {
        if (!Settings.Enabled || object->GetEntry() != ControllerEntry)
            return false;
        ClearGossipMenuFor(player);
        PlayerState& playerState = StateFor(player);
        playerState.MenuController.Clear();
        playerState.MenuCampId = 0;
        Camp* camp = FindCamp(FindState(player->FindMap()), object->GetGUID());
        if (!camp || NowMs() >= camp->ExpiresAt ||
            !Accessible(player, *camp, player->FindMap()->GetGameObject(camp->Fire)) ||
            !player->IsWithinDistInMap(object, Settings.ContributionDistance))
        {
            Message(player, "This campsite is not within reach.");
            return true;
        }
        playerState.MenuController = object->GetGUID();
        playerState.MenuCampId = camp->Id;
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Contribute Incense Candle (Herbalism 20; 1 Peacebloom + 1 Silverleaf)", GOSSIP_SENDER_MAIN, 1);
        uint64 const expires = ReadSettingPair(player, 0);
        if (CooldownActive(expires, NowSeconds()))
            ChatHandler(player->GetSession()).PSendSysMessage("Camping contribution available in {} seconds.",
                expires - NowSeconds());
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, object->GetGUID());
        return true;
    }

    bool CanGameObjectGossipSelect(Player* player, GameObject* object, uint32 sender, uint32 action) override
    {
        if (!Settings.Enabled || object->GetEntry() != ControllerEntry)
            return false;
        CloseGossipMenuFor(player);
        PlayerState& playerState = StateFor(player);
        MapState* state = FindState(player->FindMap());
        if (state && sender == GOSSIP_SENDER_MAIN && action == 1 &&
            playerState.MenuController == object->GetGUID() && playerState.MenuCampId)
        {
            std::lock_guard<std::mutex> lock(state->RequestsMutex);
            state->Requests.push_back({player->GetGUID(), object->GetGUID(), playerState.MenuCampId});
        }
        playerState.MenuController.Clear();
        playerState.MenuCampId = 0;
        return true;
    }
};

class camping_maps : public AllMapScript
{
public:
    camping_maps() : AllMapScript("camping_maps", {ALLMAPHOOK_ON_CREATE_MAP, ALLMAPHOOK_ON_DESTROY_MAP,
        ALLMAPHOOK_ON_MAP_UPDATE, ALLMAPHOOK_ON_PLAYER_LEAVE_ALL}) { }

    void OnCreateMap(Map* map) override
    {
        map->CustomData.GetDefault<MapState>(MapStateKey);
    }

    void OnDestroyMap(Map* map) override
    {
        map->CustomData.Erase(MapStateKey);
    }

    void OnMapUpdate(Map* map, uint32) override
    {
        if (Settings.Enabled)
            UpdateMap(map);
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        if (PlayerState* state = player->CustomData.Get<PlayerState>(PlayerStateKey))
        {
            if (state->ActiveFire)
                if (MapState* mapState = FindState(map))
                {
                    std::lock_guard<std::mutex> lock(mapState->RequestsMutex);
                    mapState->Removals.push_back(state->ActiveFire);
                }
            state->ActiveFire.Clear();
            state->MenuController.Clear();
            state->MenuCampId = 0;
            state->Rest.Reset();
        }
    }
};

class camping_players : public PlayerScript
{
public:
    camping_players() : PlayerScript("camping_players", {PLAYERHOOK_ON_BEFORE_UPDATE}) { }

    void OnPlayerBeforeUpdate(Player* player, uint32) override
    {
        PlayerState* state = player->CustomData.Get<PlayerState>(PlayerStateKey);
        if (Settings.Enabled && state && state->Rest.CampId && (!RestEligible(player) ||
            player->GetPhaseMask() != state->RestPhase ||
            player->GetExactDist(&state->RestPosition) > 0.01f))
            state->Rest.Reset();
    }
};

class camping_spells : public AllSpellScript
{
public:
    camping_spells() : AllSpellScript("camping_spells",
        {ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CALCULATED_TARGET}) { }

    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        if (!Settings.Enabled || result != SPELL_CAST_OK || spell->GetSpellInfo()->Id != FireSpell)
            return;
        if (Player* player = spell->GetCaster()->ToPlayer())
            if (!PlacementAllowed(player, player))
                result = SPELL_FAILED_NOT_HERE;
    }

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo&) override
    {
        if (!Settings.Enabled || !target->IsPlayer())
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id);
        if (!root)
            root = spell->GetSpellInfo()->Id;
        if (std::find(IntellectFamilies.begin(), IntellectFamilies.end(), root) != IntellectFamilies.end())
            if (Aura* aura = CampingAura(target->ToPlayer()))
                aura->Remove();
    }
};
}

void AddSC_coa_camping()
{
    new camping_world();
    new camping_objects();
    new camping_maps();
    new camping_players();
    new camping_spells();
}
