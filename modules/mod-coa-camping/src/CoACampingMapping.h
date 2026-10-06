#ifndef COA_CAMPING_MAPPING_H
#define COA_CAMPING_MAPPING_H

#include <array>
#include <cstdint>

namespace CoACamping
{
inline constexpr std::uint32_t FireSpell = 818;
inline constexpr std::uint32_t FireEntry = 29784;
inline constexpr std::uint32_t FireDisplay = 192;
inline constexpr std::uint32_t ControllerEntry = 9500200;
inline constexpr std::uint32_t ControllerDisplay = 345;
inline constexpr std::uint32_t CandleEntry = 9500201;
inline constexpr std::uint32_t CandleDisplay = 100;
inline constexpr std::uint32_t RewardSpell = 1459;
inline constexpr std::uint8_t RewardEffectMask = 1;
inline constexpr std::int32_t RewardIntellect = 2;
inline constexpr std::uint32_t Herbalism = 182;
inline constexpr std::uint32_t RequiredHerbalism = 20;
inline constexpr char CooldownSettings[] = "core.coa.camping";
inline constexpr char MapStateKey[] = "mod.coa.camping.map";
inline constexpr char PlayerStateKey[] = "mod.coa.camping.player";
inline constexpr std::array<std::uint32_t, 4> IntellectFamilies = {1459, 23028, 61024, 61316};

struct Material
{
    std::uint32_t Item;
    std::uint32_t Count;
};

inline constexpr std::array<Material, 2> CandleMaterials = {{{2447, 1}, {765, 1}}};
}

#endif
