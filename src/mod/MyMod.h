#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/world/level/BlockPos.h"

class Actor;
class ActorDamageSource;
class ItemStack;
class Player;
class BlockSource;

namespace enchant_forge {

struct EnchantDefinition {
    std::string id;
    std::string name;
    std::string category;
    int         maxLevel = 1;
    double      weight   = 1.0;
};

struct PluginConfig {
    int                            version                = 3;
    int                            effectRefreshTicks     = 100;
    int                            cacheRefreshTicks      = 5;
    int                            iceDurationSeconds     = 6;
    int                            webDurationSeconds     = 8;
    int                            maxVanillaBreakthrough = 8;
    bool                           announceTriggers       = false;
    std::vector<EnchantDefinition> enchants{
        {"lava_walker", "岩浆行者", "boots", 1, 8.0},
        {"night_vision", "夜视镜", "helmet", 1, 8.0},
        {"lightning", "雷击", "sword", 5, 6.0},
        {"lava_guard", "岩浆保护", "chestplate", 1, 8.0},
        {"return_twice", "我回来了", "totem", 1, 3.0},
        {"skyward", "欲上青天", "leggings", 1, 3.0},
        {"ice", "寒冰", "sword", 5, 6.0},
        {"ghost", "鬼影", "sword", 3, 5.0},
        {"volley", "万箭齐发", "bow", 1, 4.0},
        {"immunity", "百毒不侵", "armor", 1, 5.0},
        {"web_bind", "蛛网束缚", "bow", 1, 4.0},
        {"life_boost", "生命提升", "chestplate", 3, 5.0},
        {"blood_demon", "血魔", "sword", 1, 3.0},
        {"smelter", "锻造者", "pickaxe", 1, 6.0},
        {"memory_strike", "识忆", "mace", 1, 4.0},
        {"cold_start", "冷启动", "mace", 1, 3.0},
        {"ghost_hand", "鬼手", "sword", 1, 1.0},
        {"machete", "柴刀", "axe", 1, 2.0},
        {"ghost_domain", "鬼域", "standalone", 6, 2.0},
        {"last_stand", "屹立不倒", "chestplate", 1, 3.0},
        {"limitless_void", "无量空处", "sword", 1, 1.0},
    };
};

struct EquipmentCache {
    std::string                         fingerprint;
    std::unordered_map<std::string, int> held;
    std::array<std::unordered_map<std::string, int>, 4> armor;
    int                                 immunityPieces = 0;
    bool                                grantedFlight  = false;
    bool                                grantedNightVision = false;
    bool                                grantedLifeBoost   = false;
    int                                 grantedLifeBoostLevel = 0;
};

struct TemporaryBlock {
    BlockSource* source = nullptr;
    BlockPos     pos;
    std::string  replacement;
    std::uint64_t expiresAt = 0;
};

struct PendingDamage {
    std::int64_t  victimId = 0;
    std::uint64_t applyAt  = 0;
    float         damage   = 0.0f;
};

class MyMod {
public:
    static MyMod& getInstance();

    MyMod();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod&                         mSelf;
    PluginConfig                                mConfig;
    std::vector<ll::event::ListenerPtr>         mListeners;
    std::unordered_map<std::string, EquipmentCache> mCache;
    std::unordered_map<std::string, std::uint64_t>  mLastBowUseTick;
    std::unordered_map<std::string, std::unordered_map<std::string, int>> mBowCache;
    std::unordered_map<std::string, std::unordered_set<unsigned int>> mIgnoredEffects;
    std::unordered_map<std::string, std::string>       mColdStartItems;
    std::unordered_map<std::string, std::uint64_t>     mGhostDomainUntil;
    std::vector<TemporaryBlock>                  mTemporaryBlocks;
    std::vector<PendingDamage>                   mPendingDamage;
    std::mt19937_64                              mRandom;
    std::uint64_t                                mTick = 0;

    void registerCommands();
    void registerEvents();
    void loadConfig();

    void refreshCache(Player& player, bool force = false);
    void tickPlayer(Player& player);
    void handleHurt(Actor& victim, ActorDamageSource const& source, float& damage, bool& cancel);

    void openMainMenu(Player& player);
    void openApplyScrollMenu(Player& player);
    void openRemoveEnchantMenu(Player& player);
    void openCraftMenu(Player& player);
    void openUpgradeMenu(Player& player);
    void openAdminMenu(Player& player);
    void openAdminCustomEnchantMenu(Player& player);
    void openAdminVanillaEnchantMenu(Player& player);
    void openAdminWeightMenu(Player& player);

    [[nodiscard]] EnchantDefinition const* findDefinition(std::string const& id) const;
};

} // namespace enchant_forge
