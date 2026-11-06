#include "mod/MyMod.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

#include <Windows.h>

#include "fmt/format.h"

#include "ll/api/Config.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/entity/ActorHurtEvent.h"
#include "ll/api/event/player/PlayerDestroyBlockEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/player/PlayerRespawnEvent.h"
#include "ll/api/event/player/PlayerUseItemEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/mod/RegisterHelper.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/safety/RedactableString.h"
#include "mc/server/ServerLevel.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandUtils.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorEvent.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/MobEffect.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/item/enchanting/ItemEnchants.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/legacy/ActorUniqueID.h"

namespace enchant_forge {
namespace {

constexpr std::string_view kEnchantMarker = "[EF:";
constexpr std::string_view kScrollMarker  = "[EFSCROLL:";
constexpr std::string_view kUpgradeMarker = "[EFUPGRADE]";
constexpr std::string_view kUsesMarker    = "[EFUSES:return_twice:";
constexpr std::int64_t     kRandomScrollPrice = 100000;
constexpr int              kLongEffectTicks   = 20 * 60 * 60;
constexpr int              kPermanentEffectTicks = std::numeric_limits<int>::max();

std::array<EnchantDefinition, 7> const kVersion3Enchants{{
    {"memory_strike", "识忆", "mace", 1, 4.0},
    {"cold_start", "冷启动", "mace", 1, 3.0},
    {"ghost_hand", "鬼手", "sword", 1, 1.0},
    {"machete", "柴刀", "axe", 1, 2.0},
    {"ghost_domain", "鬼域", "standalone", 6, 2.0},
    {"last_stand", "屹立不倒", "chestplate", 1, 3.0},
    {"limitless_void", "无量空处", "sword", 1, 1.0},
}};

using LLMoneyGetFn    = std::int64_t (*)(std::string);
using LLMoneyAddFn    = bool (*)(std::string, std::int64_t);
using LLMoneyReduceFn = bool (*)(std::string, std::int64_t);

struct LLMoneyApi {
    LLMoneyGetFn    get    = nullptr;
    LLMoneyAddFn    add    = nullptr;
    LLMoneyReduceFn reduce = nullptr;

    [[nodiscard]] bool available() const { return get && add && reduce; }
};

LLMoneyApi getLLMoneyApi() {
    auto module = GetModuleHandleW(L"LegacyMoney.dll");
    if (!module) return {};
    return {
        reinterpret_cast<LLMoneyGetFn>(GetProcAddress(module, "LLMoney_Get")),
        reinterpret_cast<LLMoneyAddFn>(GetProcAddress(module, "LLMoney_Add")),
        reinterpret_cast<LLMoneyReduceFn>(GetProcAddress(module, "LLMoney_Reduce")),
    };
}

void playUiClick(Player& player) {
    player.getLevel().playSound("random.click", player.getPosition(), 0.6f, 1.2f);
}

void playUiSuccess(Player& player) {
    player.getLevel().playSound("random.levelup", player.getPosition(), 0.8f, 1.1f);
}

std::optional<size_t> readFormIndex(
    ll::form::CustomFormElementResult const& value,
    std::vector<std::string> const&          options
) {
    if (auto index = std::get_if<uint64>(&value)) return static_cast<size_t>(*index);
    if (auto number = std::get_if<double>(&value); number && std::isfinite(*number) && *number >= 0.0
        && std::floor(*number) == *number && *number <= static_cast<double>(std::numeric_limits<size_t>::max())) {
        return static_cast<size_t>(*number);
    }
    // LeviLamina 26.10 returns the selected dropdown option text, not its numeric index.
    if (auto text = std::get_if<std::string>(&value)) {
        auto match = std::ranges::find(options, *text);
        if (match != options.end()) return static_cast<size_t>(std::distance(options.begin(), match));
        try {
            size_t consumed = 0;
            auto index = std::stoull(*text, &consumed);
            if (consumed == text->size()) return static_cast<size_t>(index);
        } catch (...) {}
    }
    return std::nullopt;
}

std::optional<double> readFormNumber(ll::form::CustomFormElementResult const& value) {
    if (auto number = std::get_if<double>(&value); number && std::isfinite(*number)) return *number;
    if (auto number = std::get_if<uint64>(&value)) return static_cast<double>(*number);
    return std::nullopt;
}

struct ScrollInfo {
    int         slot  = -1;
    std::string id;
    int         level = 1;
};

bool endsWith(std::string const& value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::pair<std::string, int>> parseMarker(std::string const& line, std::string_view marker) {
    auto begin = line.find(marker);
    if (begin == std::string::npos) return std::nullopt;
    begin += marker.size();
    auto end = line.find(']', begin);
    if (end == std::string::npos) return std::nullopt;
    auto body = line.substr(begin, end - begin);
    auto sep  = body.rfind(':');
    if (sep == std::string::npos) return std::nullopt;
    try {
        int level = std::stoi(body.substr(sep + 1));
        if (level < 1) return std::nullopt;
        return std::pair{body.substr(0, sep), level};
    } catch (...) {
        return std::nullopt;
    }
}

bool matchesCategory(ItemStack const& item, std::string const& category);

std::unordered_map<std::string, int> readCustomEnchants(ItemStack const& item) {
    std::unordered_map<std::string, int> result;
    if (item.isNull()) return result;
    for (auto const& line : item.getCustomLore()) {
        if (auto parsed = parseMarker(line, kEnchantMarker)) result[parsed->first] = parsed->second;
    }
    return result;
}

std::unordered_map<std::string, int> readValidCustomEnchants(
    ItemStack const& item,
    std::vector<EnchantDefinition> const& definitions
) {
    auto result = readCustomEnchants(item);
    for (auto it = result.begin(); it != result.end();) {
        auto definition = std::ranges::find_if(definitions, [&it](auto const& value) { return value.id == it->first; });
        if (definition == definitions.end() || !matchesCategory(item, definition->category)) {
            it = result.erase(it);
            continue;
        }
        it->second = std::clamp(it->second, 1, std::max(1, definition->maxLevel));
        ++it;
    }
    return result;
}

std::optional<std::pair<std::string, int>> readScroll(ItemStack const& item) {
    if (item.isNull()) return std::nullopt;
    for (auto const& line : item.getCustomLore()) {
        if (auto parsed = parseMarker(line, kScrollMarker)) return parsed;
    }
    return std::nullopt;
}

bool isUpgradeScroll(ItemStack const& item) {
    if (item.isNull()) return false;
    auto lore = item.getCustomLore();
    return std::ranges::any_of(lore, [](std::string const& line) { return line.find(kUpgradeMarker) != std::string::npos; });
}

int readReviveUses(ItemStack const& item) {
    if (item.isNull()) return 0;
    for (auto const& line : item.getCustomLore()) {
        auto begin = line.find(kUsesMarker);
        if (begin == std::string::npos) continue;
        begin += kUsesMarker.size();
        auto end = line.find(']', begin);
        if (end == std::string::npos) continue;
        try {
            return std::max(0, std::stoi(line.substr(begin, end - begin)));
        } catch (...) {}
    }
    return 0;
}

void writeCustomLore(ItemStack& item, std::vector<std::string> const& lore) {
    if (lore.empty()) {
        item.clearCustomLore();
        return;
    }
    item.setCustomLore(lore);
}

void setReviveUses(ItemStack& item, int uses) {
    auto lore = item.getCustomLore();
    std::erase_if(lore, [](std::string const& line) { return line.find(kUsesMarker) != std::string::npos; });
    if (uses > 0) lore.emplace_back(fmt::format("§8[EFUSES:return_twice:{}]", uses));
    writeCustomLore(item, lore);
}

void setCustomName(ItemStack& item, std::string name) {
    Bedrock::Safety::RedactableString text;
    text.set(std::move(name));
    item.setCustomName(text);
}

bool matchesCategory(ItemStack const& item, std::string const& category) {
    if (item.isNull()) return false;
    auto type = item.getTypeName();
    if (category == "any") return true;
    if (category == "sword") return endsWith(type, "_sword");
    if (category == "bow") return type == "minecraft:bow";
    if (category == "pickaxe") return endsWith(type, "_pickaxe");
    if (category == "axe") return endsWith(type, "_axe");
    if (category == "mace") return type == "minecraft:mace";
    if (category == "helmet") return endsWith(type, "_helmet");
    if (category == "chestplate") return endsWith(type, "_chestplate");
    if (category == "leggings") return endsWith(type, "_leggings");
    if (category == "boots") return endsWith(type, "_boots");
    if (category == "totem") return type == "minecraft:totem_of_undying";
    if (category == "armor") {
        return endsWith(type, "_helmet") || endsWith(type, "_chestplate") || endsWith(type, "_leggings")
            || endsWith(type, "_boots");
    }
    return false;
}

void applyCustomEnchant(ItemStack& item, EnchantDefinition const& definition, int level) {
    auto lore = item.getCustomLore();
    auto key  = fmt::format("[EF:{}:", definition.id);
    std::erase_if(lore, [&key](std::string const& line) { return line.find(key) != std::string::npos; });
    lore.emplace_back(fmt::format("§r§b{} {} §8[EF:{}:{}]", definition.name, level, definition.id, level));
    writeCustomLore(item, lore);
    if (definition.id == "return_twice") setReviveUses(item, 2);
}

bool removeCustomEnchant(ItemStack& item, std::string const& id) {
    auto lore = item.getCustomLore();
    auto enchantKey = fmt::format("[EF:{}:", id);
    auto oldSize = lore.size();
    std::erase_if(lore, [&enchantKey, &id](std::string const& line) {
        return line.find(enchantKey) != std::string::npos
            || (id == "return_twice" && line.find(kUsesMarker) != std::string::npos);
    });
    if (lore.size() == oldSize) return false;
    writeCustomLore(item, lore);
    return std::ranges::none_of(item.getCustomLore(), [&enchantKey, &id](std::string const& line) {
        return line.find(enchantKey) != std::string::npos
            || (id == "return_twice" && line.find(kUsesMarker) != std::string::npos);
    });
}

ItemStack makeScroll(EnchantDefinition const& definition, int level) {
    ItemStack scroll = CommandUtils::createItemStack("minecraft:paper", 1, 0);
    setCustomName(scroll, fmt::format("§r§b{}卷轴 {}", definition.name, level));
    scroll.setCustomLore({
        fmt::format("§7适用: {}", definition.category),
        fmt::format("§8[EFSCROLL:{}:{}]", definition.id, level),
    });
    return scroll;
}

ItemStack makeUpgradeScroll(int count) {
    ItemStack scroll = CommandUtils::createItemStack("minecraft:paper", count, 0);
    setCustomName(scroll, "§r§d原版附魔突破卷轴");
    scroll.setCustomLore({"§7将已有原版附魔提升一级，最高 8 级", "§8[EFUPGRADE]"});
    return scroll;
}

void notifyInventory(Player& player, int slot) {
    player.getInventory().setContainerChanged(slot);
    player.refreshInventory();
}

bool removeOneAt(Player& player, int slot) {
    auto item = player.getInventory().getItemNonConst(slot);
    if (!item || item->isNull()) return false;
    item->remove(1);
    notifyInventory(player, slot);
    return true;
}

int countMatching(Player& player, std::function<bool(ItemStack const&)> const& predicate) {
    int total = 0;
    auto& inv = player.getInventory();
    for (int slot = 0; slot < inv.getContainerSize(); ++slot) {
        auto const& item = inv.getItem(slot);
        if (predicate(item)) total += item.mCount;
    }
    return total;
}

bool removeMatching(Player& player, int count, std::function<bool(ItemStack const&)> const& predicate) {
    auto& inv = player.getInventory();
    for (int slot = 0; slot < inv.getContainerSize() && count > 0; ++slot) {
        auto item = inv.getItemNonConst(slot);
        if (!item || !predicate(*item)) continue;
        int take = std::min(count, static_cast<int>(item->mCount));
        item->remove(take);
        inv.setContainerChanged(slot);
        count -= take;
    }
    player.refreshInventory();
    return count == 0;
}

std::string itemFingerprint(ItemStack const& item) {
    if (item.isNull()) return "-";
    std::string value = item.getTypeName();
    for (auto const& line : item.getCustomLore()) value.append("|").append(line);
    return value;
}

int levelOf(std::unordered_map<std::string, int> const& enchants, std::string_view id) {
    auto it = enchants.find(std::string{id});
    return it == enchants.end() ? 0 : it->second;
}

void addEffect(Actor& actor, MobEffect* effect, int durationTicks, int amplifier, bool visible = false) {
    if (!effect) return;
    MobEffectInstance instance(static_cast<unsigned int>(effect->mId));
    instance.mDuration->mValue = durationTicks;
    instance.mAmplifier       = amplifier;
    instance.mAmbient         = true;
    instance.mEffectVisible   = visible;
    actor.addEffect(instance);
}

bool setVanillaEnchantLevel(ItemStack& item, Enchant::Type type, int level) {
    auto existing = item.constructItemEnchantsFromUserData();
    bool found = false;
    for (auto& group : existing.mItemEnchants) {
        for (auto& enchantment : *group) {
            if (enchantment.mEnchantType == type) {
                enchantment.mLevel = level;
                found = true;
            }
        }
    }
    if (!found) {
        return EnchantUtils::applyEnchant(item, type, level, true)
            && EnchantUtils::getEnchantLevel(type, item) == level;
    }
    item.saveEnchantsToUserData(existing);
    return EnchantUtils::getEnchantLevel(type, item) == level;
}

bool placeBlock(BlockSource& source, BlockPos const& pos, std::string const& type) {
    auto block = Block::tryGetFromRegistry(HashedString(type));
    return block && source.setBlockAndRetainCompatibleBlockActor(pos, *block, 3);
}

std::array<std::pair<Enchant::Type, std::string_view>, 42> const kVanillaEnchants{{
    {Enchant::Type::Protection, "保护"}, {Enchant::Type::FireProtection, "火焰保护"},
    {Enchant::Type::FeatherFalling, "摔落缓冲"}, {Enchant::Type::BlastProtection, "爆炸保护"},
    {Enchant::Type::ProjectileProtection, "弹射物保护"}, {Enchant::Type::Thorns, "荆棘"},
    {Enchant::Type::Respiration, "水下呼吸"}, {Enchant::Type::DepthStrider, "深海探索者"},
    {Enchant::Type::AquaAffinity, "水下速掘"}, {Enchant::Type::Sharpness, "锋利"},
    {Enchant::Type::Smite, "亡灵杀手"}, {Enchant::Type::BaneOfArthropods, "节肢杀手"},
    {Enchant::Type::Knockback, "击退"}, {Enchant::Type::FireAspect, "火焰附加"},
    {Enchant::Type::Looting, "抢夺"}, {Enchant::Type::Efficiency, "效率"},
    {Enchant::Type::SilkTouch, "精准采集"}, {Enchant::Type::Unbreaking, "耐久"},
    {Enchant::Type::Fortune, "时运"}, {Enchant::Type::Power, "力量"},
    {Enchant::Type::Punch, "冲击"}, {Enchant::Type::Flame, "火矢"},
    {Enchant::Type::Infinity, "无限"}, {Enchant::Type::LuckOfTheSea, "海之眷顾"},
    {Enchant::Type::Lure, "饵钓"}, {Enchant::Type::FrostWalker, "冰霜行者"},
    {Enchant::Type::Mending, "经验修补"}, {Enchant::Type::CurseOfBinding, "绑定诅咒"},
    {Enchant::Type::CurseOfVanishing, "消失诅咒"}, {Enchant::Type::Impaling, "穿刺"},
    {Enchant::Type::Riptide, "激流"}, {Enchant::Type::Loyalty, "忠诚"},
    {Enchant::Type::Channeling, "引雷"}, {Enchant::Type::Multishot, "多重射击"},
    {Enchant::Type::Piercing, "穿透"}, {Enchant::Type::QuickCharge, "快速装填"},
    {Enchant::Type::SoulSpeed, "灵魂疾行"}, {Enchant::Type::SwiftSneak, "迅捷潜行"},
    {Enchant::Type::WindBurst, "风爆"}, {Enchant::Type::Density, "致密"},
    {Enchant::Type::Breach, "破甲"}, {Enchant::Type::Lunge, "突刺"},
}};

} // namespace

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

MyMod::MyMod() : mSelf(*ll::mod::NativeMod::current()), mRandom(std::random_device{}()) {}

bool MyMod::load() {
    loadConfig();
    getSelf().getLogger().info("EnchantForge 配置已加载，共 {} 个扩展附魔", mConfig.enchants.size());
    return true;
}

bool MyMod::enable() {
    registerCommands();
    registerEvents();
    getSelf().getLogger().info("EnchantForge 已启用，使用 /ef 打开总菜单");
    return true;
}

bool MyMod::disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto const& listener : mListeners) bus.removeListener(listener);
    mListeners.clear();
    mCache.clear();
    mBowCache.clear();
    mColdStartItems.clear();
    mGhostDomainUntil.clear();
    mTemporaryBlocks.clear();
    mPendingDamage.clear();
    return true;
}

void MyMod::loadConfig() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path)) ll::config::saveConfig(mConfig, path);
    bool changed = false;
    for (auto const& required : kVersion3Enchants) {
        auto exists = std::ranges::any_of(mConfig.enchants, [&required](auto const& entry) { return entry.id == required.id; });
        if (!exists) {
            mConfig.enchants.push_back(required);
            changed = true;
        }
    }
    if (mConfig.version < 3) {
        mConfig.version = 3;
        changed = true;
    }
    mConfig.cacheRefreshTicks  = std::max(1, mConfig.cacheRefreshTicks);
    mConfig.effectRefreshTicks = std::max(20, mConfig.effectRefreshTicks);
    for (auto& definition : mConfig.enchants) definition.maxLevel = std::max(1, definition.maxLevel);
    if (changed) ll::config::saveConfig(mConfig, path);
}

EnchantDefinition const* MyMod::findDefinition(std::string const& id) const {
    auto it = std::ranges::find_if(mConfig.enchants, [&id](auto const& entry) { return entry.id == id; });
    return it == mConfig.enchants.end() ? nullptr : &*it;
}

void MyMod::registerCommands() {
    using namespace ll::command;
    auto& command = CommandRegistrar::getServerInstance().getOrCreateCommand(
        "ef",
        "打开 EnchantForge 附魔总菜单",
        CommandPermissionLevel::Any
    );
    command.alias("enchantforge");
    command.overload().execute([this](CommandOrigin const& origin, CommandOutput& output) {
        auto* actor = origin.getEntity();
        if (!actor || !actor->isPlayer()) {
            output.error("该命令只能由玩家执行");
            return;
        }
        auto& player = static_cast<Player&>(*actor);
        openMainMenu(player);
        output.success("EnchantForge 菜单已打开");
    });
}

void MyMod::registerEvents() {
    auto& bus = ll::event::EventBus::getInstance();

    mListeners.emplace_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto& event) {
        ++mTick;
        if (!mPendingDamage.empty()) {
            std::erase_if(mPendingDamage, [this, &event](PendingDamage const& entry) {
                if (entry.applyAt > mTick) return false;
                auto* victim = event.level().fetchEntity(ActorUniqueID{entry.victimId}, false);
                if (victim && victim->isAlive() && entry.damage > 0.0f) {
                    victim->hurtByCause(entry.damage, SharedTypes::Legacy::ActorDamageCause::Override);
                }
                return true;
            });
        }
        if (!mTemporaryBlocks.empty()) {
            std::erase_if(mTemporaryBlocks, [this](TemporaryBlock const& entry) {
                if (entry.expiresAt > mTick) return false;
                if (entry.source && entry.source->getBlock(entry.pos).getTypeName() == entry.replacement) {
                    placeBlock(*entry.source, entry.pos, "minecraft:air");
                }
                return true;
            });
        }
        if (mTick % static_cast<std::uint64_t>(mConfig.cacheRefreshTicks) != 0) return;
        event.level().forEachPlayer([this](Player& player) {
            tickPlayer(player);
            return true;
        });
    }));

    mListeners.emplace_back(bus.emplaceListener<ll::event::PlayerJoinEvent>([this](auto& event) {
        refreshCache(event.self(), true);
    }));

    mListeners.emplace_back(bus.emplaceListener<ll::event::PlayerRespawnEvent>([this](auto& event) {
        auto key = event.self().getUuid().asString();
        auto it  = mCache.find(key);
        if (it != mCache.end()) it->second.grantedFlight = false;
    }));

    mListeners.emplace_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([this](auto& event) {
        auto key = event.self().getUuid().asString();
        mCache.erase(key);
        mBowCache.erase(key);
        mLastBowUseTick.erase(key);
        mIgnoredEffects.erase(key);
        mColdStartItems.erase(key);
    }));

    mListeners.emplace_back(bus.emplaceListener<ll::event::PlayerUseItemEvent>([this](auto& event) {
        if (auto scroll = readScroll(event.item()); scroll && scroll->first == "ghost_domain") {
            auto& player = event.self();
            auto key = player.getUuid().asString();
            auto active = mGhostDomainUntil.find(key);
            if (active != mGhostDomainUntil.end() && active->second > mTick) {
                player.sendMessage("§c鬼域已经开启，此过程中不可再次开启或关闭鬼域");
                event.cancel();
                return;
            }
            auto definition = findDefinition("ghost_domain");
            int maxLevel = definition ? std::max(1, definition->maxLevel) : 6;
            int level = std::clamp(scroll->second, 1, maxLevel);
            auto duration = static_cast<std::uint64_t>(level * 10 * 20);
            mGhostDomainUntil[key] = mTick + duration;
            event.item().remove(1);
            player.refreshInventory();
            addEffect(player, MobEffect::INVISIBILITY(), static_cast<int>(duration), 0, false);
            player.sendMessage("§a你已开启鬼域，此过程中不可关闭鬼域");
            event.cancel();
            return;
        }
        if (event.item().getTypeName() != "minecraft:bow") return;
        refreshCache(event.self(), true);
        auto key             = event.self().getUuid().asString();
        mLastBowUseTick[key] = mTick;
        mBowCache[key]       = mCache[key].held;
    }));

    mListeners.emplace_back(bus.emplaceListener<ll::event::ActorHurtEvent>([this](auto& event) {
        bool cancel = false;
        handleHurt(event.self(), event.source(), event.damage(), cancel);
        if (cancel) event.cancel();
    }, ll::event::EventPriority::High));

    mListeners.emplace_back(bus.emplaceListener<ll::event::PlayerDestroyBlockEvent>([this](auto& event) {
        auto& player = event.self();
        refreshCache(player);
        auto key = player.getUuid().asString();
        if (levelOf(mCache[key].held, "smelter") <= 0) return;

        auto& source = player.getDimensionBlockSource();
        auto type    = source.getBlock(event.pos()).getTypeName();
        struct SmeltDrop {
            std::string output;
            int         minBaseCount;
            int         maxBaseCount;
            bool        affectedByFortune;
        };
        static std::unordered_map<std::string, SmeltDrop> const drops{
            {"minecraft:iron_ore", {"minecraft:iron_ingot", 1, 1, true}},
            {"minecraft:deepslate_iron_ore", {"minecraft:iron_ingot", 1, 1, true}},
            {"minecraft:gold_ore", {"minecraft:gold_ingot", 1, 1, true}},
            {"minecraft:deepslate_gold_ore", {"minecraft:gold_ingot", 1, 1, true}},
            {"minecraft:nether_gold_ore", {"minecraft:gold_ingot", 1, 1, true}},
            {"minecraft:copper_ore", {"minecraft:copper_ingot", 2, 5, true}},
            {"minecraft:deepslate_copper_ore", {"minecraft:copper_ingot", 2, 5, true}},
            {"minecraft:ancient_debris", {"minecraft:netherite_scrap", 1, 1, false}},
        };
        auto it = drops.find(type);
        if (it == drops.end()) return;

        auto const& held = player.getInventory().getItem(player.getSelectedItemSlot());
        if (EnchantUtils::getEnchantLevel(Enchant::Type::SilkTouch, held) > 0) return;
        int count = std::uniform_int_distribution<int>(it->second.minBaseCount, it->second.maxBaseCount)(mRandom);
        if (it->second.affectedByFortune) {
            int fortune = std::max(0, EnchantUtils::getEnchantLevel(Enchant::Type::Fortune, held));
            if (fortune > 0) {
                // Vanilla ore fortune multiplier: max(0, random(0..level+1) - 1) + 1.
                int bonus = std::uniform_int_distribution<int>(0, std::min(fortune, 255) + 1)(mRandom) - 1;
                count *= std::max(0, bonus) + 1;
            }
        }
        count = std::clamp(count, 1, 64);
        event.cancel();
        placeBlock(source, event.pos(), "minecraft:air");
        ItemStack ingot = CommandUtils::createItemStack(it->second.output, count, 0);
        BlockType::popResource(source, event.pos(), ingot);
    }));
}

void MyMod::refreshCache(Player& player, bool force) {
    auto key = player.getUuid().asString();
    auto& out = mCache[key];
    auto& inv = player.getInventory();
    auto const& held = inv.getItem(player.getSelectedItemSlot());
    std::string fingerprint = itemFingerprint(held);
    for (int slot = 0; slot < 4; ++slot) {
        fingerprint.append("#").append(itemFingerprint(player.getArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(slot))));
    }
    if (!force && fingerprint == out.fingerprint) return;
    out.fingerprint = std::move(fingerprint);
    out.held        = readValidCustomEnchants(held, mConfig.enchants);
    out.immunityPieces = 0;
    for (int slot = 0; slot < 4; ++slot) {
        out.armor[slot] = readValidCustomEnchants(
            player.getArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(slot)),
            mConfig.enchants
        );
        if (levelOf(out.armor[slot], "immunity") > 0) ++out.immunityPieces;
    }
}

void MyMod::tickPlayer(Player& player) {
    refreshCache(player);
    auto key   = player.getUuid().asString();
    auto& data = mCache[key];
    auto const& held = player.getInventory().getItem(player.getSelectedItemSlot());

    if (levelOf(data.held, "cold_start") <= 0
        || EnchantUtils::getEnchantLevel(Enchant::Type::WindBurst, held) <= 0) {
        mColdStartItems.erase(key);
    }

    if (auto active = mGhostDomainUntil.find(key); active != mGhostDomainUntil.end()) {
        auto* invisibility = MobEffect::INVISIBILITY();
        if (active->second <= mTick) {
            if (invisibility) player.removeEffect(static_cast<int>(invisibility->mId));
            mGhostDomainUntil.erase(active);
        } else if (invisibility && !player.hasEffect(*invisibility)) {
            auto remaining = std::min<std::uint64_t>(active->second - mTick, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
            addEffect(player, invisibility, static_cast<int>(remaining), 0, false);
        }
    }

    bool hasFlight = levelOf(data.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Legs)], "skyward") > 0;
    // Keep the permission, not the movement state, in sync. Login/respawn
    // ability packets can overwrite an earlier grant, so check the actual
    // server-side permission instead of trusting only our cache. Once MayFly
    // is present this sends nothing and leaves Flying, Elytra and falling
    // entirely to vanilla movement, avoiding rubber-banding.
    if (hasFlight) {
        if (!player.canUseAbility(AbilitiesIndex::MayFly)) {
            player.setAbility(AbilitiesIndex::MayFly, true);
        }
        data.grantedFlight = true;
    } else if (data.grantedFlight && player.getPlayerGameType() != GameType::Creative
               && player.getPlayerGameType() != GameType::Spectator) {
        player.setAbility(AbilitiesIndex::Flying, false);
        player.setAbility(AbilitiesIndex::MayFly, false);
        data.grantedFlight = false;
    }

    bool refreshEffects = mTick % static_cast<std::uint64_t>(mConfig.effectRefreshTicks) == 0;
    bool hasNightVision = levelOf(data.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Head)], "night_vision") > 0;
    if (hasNightVision && (refreshEffects || !data.grantedNightVision)) {
        addEffect(player, MobEffect::NIGHT_VISION(), kLongEffectTicks, 0);
        data.grantedNightVision = true;
    } else if (!hasNightVision && data.grantedNightVision) {
        if (MobEffect::NIGHT_VISION()) player.removeEffect(static_cast<int>(MobEffect::NIGHT_VISION()->mId));
        data.grantedNightVision = false;
    }

    int lifeLevel = levelOf(data.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Torso)], "life_boost");
    auto* healthBoost = MobEffect::HEALTH_BOOST();
    bool healthBoostMissing = healthBoost && !player.hasEffect(*healthBoost);
    if (lifeLevel > 0 && (!data.grantedLifeBoost || data.grantedLifeBoostLevel != lifeLevel ||
                          (refreshEffects && healthBoostMissing))) {
        addEffect(player, healthBoost, kPermanentEffectTicks, lifeLevel - 1);
        data.grantedLifeBoost = true;
        data.grantedLifeBoostLevel = lifeLevel;
    } else if (lifeLevel <= 0 && data.grantedLifeBoost) {
        if (healthBoost) player.removeEffect(static_cast<int>(healthBoost->mId));
        data.grantedLifeBoost = false;
        data.grantedLifeBoostLevel = 0;
    }

    if (levelOf(data.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Feet)], "lava_walker") > 0) {
        auto feet = player.getFeetPos();
        auto& source = player.getDimensionBlockSource();
        int y = static_cast<int>(std::floor(feet.y)) - 1;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                BlockPos pos(static_cast<int>(std::floor(feet.x)) + dx, y, static_cast<int>(std::floor(feet.z)) + dz);
                auto const& block = source.getBlock(pos);
                if (block.getTypeName() == "minecraft:lava" || source.getLiquidBlock(pos).getTypeName() == "minecraft:lava") {
                    placeBlock(source, pos, "minecraft:basalt");
                }
            }
        }
    }

    if (data.immunityPieces > 0) {
        std::array<MobEffect*, 13> harmful{
            MobEffect::POISON(), MobEffect::FATAL_POISON(), MobEffect::WITHER(), MobEffect::BLINDNESS(),
            MobEffect::DARKNESS(), MobEffect::CONFUSION(), MobEffect::MOVEMENT_SLOWDOWN(), MobEffect::DIG_SLOWDOWN(),
            MobEffect::WEAKNESS(), MobEffect::HUNGER(), MobEffect::BAD_OMEN(), MobEffect::LEVITATION(), MobEffect::HARM(),
        };
        auto& ignored = mIgnoredEffects[key];
        std::unordered_set<unsigned int> active;
        std::uniform_real_distribution<double> chance(0.0, 100.0);
        for (auto* effect : harmful) {
            if (!effect || !player.hasEffect(*effect)) continue;
            auto id = static_cast<unsigned int>(effect->mId);
            active.insert(id);
            if (ignored.contains(id)) continue;
            if (chance(mRandom) < data.immunityPieces * 25.0) player.removeEffect(static_cast<int>(id));
            else ignored.insert(id);
        }
        std::erase_if(ignored, [&active](unsigned int id) { return !active.contains(id); });
    } else {
        mIgnoredEffects.erase(key);
    }
}

void MyMod::handleHurt(Actor& victim, ActorDamageSource const& source, float& damage, bool& cancel) {
    if (victim.isPlayer()) {
        auto& player = static_cast<Player&>(victim);
        refreshCache(player);
        auto key = player.getUuid().asString();
        auto& cache = mCache[key];
        if (source.isFire() && levelOf(cache.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Torso)], "lava_guard") > 0) {
            cancel = true;
            damage = 0.0f;
            player.setOnFire(0, false);
            return;
        }
        if (damage > 0.0f
            && levelOf(cache.armor[static_cast<int>(SharedTypes::Legacy::ArmorSlot::Torso)], "last_stand") > 0
            && player.getHealth() <= std::max(1, static_cast<int>(std::ceil(player.getMaxHealth() * 0.3f)))
            && std::uniform_real_distribution<double>(0.0, 100.0)(mRandom) < 15.0) {
            cancel = true;
            damage = 0.0f;
            int healing = std::min(6, std::max(0, player.getMaxHealth() - player.getHealth()));
            if (healing > 0) player.heal(healing);
            player.handleEntityEvent(ActorEvent::TalismanActivate, 0);
            player.getLevel().playSound("random.totem", player.getPosition(), 1.0f, 1.0f);
            player.sendMessage("§a屹立不倒触发，抵挡了本次伤害并回复生命");
            return;
        }
        if (damage >= static_cast<float>(player.getHealth())) {
            auto selectedSlot = player.getSelectedItemSlot();
            auto selected = player.getInventory().getItemNonConst(selectedSlot);
            if (selected && levelOf(readValidCustomEnchants(*selected, mConfig.enchants), "return_twice") > 0) {
                int uses = readReviveUses(*selected);
                if (uses > 0) {
                    cancel = true;
                    damage = 0.0f;
                    player.heal(player.getMaxHealth());
                    if (uses > 1) setReviveUses(*selected, uses - 1);
                    else selected->remove(1);
                    notifyInventory(player, selectedSlot);
                    refreshCache(player, true);
                    return;
                }
            }
            auto offhand = player.getOffhandSlot();
            if (levelOf(readValidCustomEnchants(offhand, mConfig.enchants), "return_twice") > 0) {
                int uses = readReviveUses(offhand);
                if (uses > 0) {
                    cancel = true;
                    damage = 0.0f;
                    player.heal(player.getMaxHealth());
                    ItemStack updated(offhand);
                    if (uses > 1) setReviveUses(updated, uses - 1);
                    else updated.remove(1);
                    player.setOffhandSlot(updated);
                    player.refreshInventory();
                    refreshCache(player, true);
                    return;
                }
            }
        }
    }

    auto cause = source.mCause;
    bool maceSmash = cause == SharedTypes::Legacy::ActorDamageCause::MaceSmash
                  && source.isEntitySource() && !source.isChildEntitySource();
    bool directMelee = (cause == SharedTypes::Legacy::ActorDamageCause::EntityAttack || maceSmash)
                    && source.isEntitySource() && !source.isChildEntitySource();
    bool projectile = cause == SharedTypes::Legacy::ActorDamageCause::Projectile && source.isChildEntitySource();
    if (!directMelee && !projectile) return;

    auto attackerId = source.getDamagingEntityUniqueID();
    auto* attackerActor = victim.getLevel().fetchEntity(attackerId, false);
    if (!attackerActor || !attackerActor->isPlayer()) return;
    auto& attacker = static_cast<Player&>(*attackerActor);
    auto key = attacker.getUuid().asString();
    bool bowHit = projectile && mLastBowUseTick.contains(key) && mTick - mLastBowUseTick[key] <= 200;
    if (projectile && !bowHit) return;
    if (!bowHit) refreshCache(attacker);
    auto const& enchants = bowHit ? mBowCache[key] : mCache[key].held;
    std::uniform_real_distribution<double> chance(0.0, 100.0);

    if (!bowHit) {
        auto const& held = attacker.getInventory().getItem(attacker.getSelectedItemSlot());
        if (maceSmash) {
            if (levelOf(enchants, "cold_start") > 0
                && EnchantUtils::getEnchantLevel(Enchant::Type::WindBurst, held) > 0) {
                auto fingerprint = itemFingerprint(held);
                auto previous = mColdStartItems.find(key);
                if (previous == mColdStartItems.end() || previous->second != fingerprint) {
                    damage *= 0.4f;
                    mColdStartItems[key] = std::move(fingerprint);
                    attacker.sendMessage("§e冷启动：首次重击伤害降低 60%");
                } else {
                    damage *= 1.3f;
                }
            }
            if (levelOf(enchants, "memory_strike") > 0 && damage > 0.0f) {
                mPendingDamage.push_back({victim.getOrCreateUniqueID().rawID, mTick + 10, damage * 0.35f});
            }
        }
        if (levelOf(enchants, "ghost_hand") > 0 && chance(mRandom) < 5.0) {
            damage = std::max(damage, 1000000.0f);
            if (mConfig.announceTriggers) attacker.sendMessage("§5鬼手触发");
        }
        if (levelOf(enchants, "machete") > 0 && victim.isPlayer() && chance(mRandom) < 3.0) {
            auto& target = static_cast<Player&>(victim);
            struct ArmorEnchantChoice {
                SharedTypes::Legacy::ArmorSlot slot;
                std::string                    id;
                int                            level;
            };
            std::vector<ArmorEnchantChoice> choices;
            for (int slotIndex = 0; slotIndex < 4; ++slotIndex) {
                auto slot = static_cast<SharedTypes::Legacy::ArmorSlot>(slotIndex);
                auto armorEnchants = readValidCustomEnchants(target.getArmor(slot), mConfig.enchants);
                for (auto const& [id, level] : armorEnchants) choices.push_back({slot, id, level});
            }
            if (!choices.empty()) {
                auto const& choice = choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(mRandom)];
                auto definition = findDefinition(choice.id);
                if (definition) {
                    ItemStack updated(target.getArmor(choice.slot));
                    if (choice.level > 1) {
                        applyCustomEnchant(updated, *definition, choice.level - 1);
                    } else {
                        removeCustomEnchant(updated, choice.id);
                        auto scroll = makeScroll(*definition, 1);
                        BlockType::popResource(target.getDimensionBlockSource(), target.getFeetBlockPos(), scroll);
                    }
                    target.setArmor(choice.slot, updated);
                    target.refreshInventory();
                    refreshCache(target, true);
                    attacker.sendMessage(fmt::format("§6柴刀触发：{} 降低至 {} 级", definition->name, std::max(0, choice.level - 1)));
                    target.sendMessage(fmt::format("§c你的 {} 被柴刀削弱", definition->name));
                }
            }
        }
        if (levelOf(enchants, "limitless_void") > 0 && victim.isPlayer() && chance(mRandom) < 5.0) {
            auto& target = static_cast<Player&>(victim);
            auto pos = target.getPosition();
            float voidY = static_cast<float>(target.getDimensionBlockSource().getMinHeight()) - 8.0f;
            target.teleport({pos.x, voidY, pos.z}, target.getDimensionId(), target.getRotation());
            attacker.sendMessage("§5无量空处触发，目标已坠入虚空");
            target.sendMessage("§5你被无量空处拖入了所在位置的虚空");
        }
        int lightning = levelOf(enchants, "lightning");
        if (lightning > 0 && chance(mRandom) < 5.0 + (lightning - 1) * 7.5) {
            auto pos = victim.getPosition();
            auto& level = static_cast<ServerLevel&>(victim.getLevel());
            ServerCommandOrigin origin("EnchantForge", level, CommandPermissionLevel::Owner, victim.getDimensionId());
            ll::command::CommandRegistrar::getServerInstance().executeCommand(
                fmt::format("summon lightning_bolt {:.2f} {:.2f} {:.2f}", pos.x, pos.y, pos.z), origin,
                CommandOutputType::Silent
            );
        }
        int ice = levelOf(enchants, "ice");
        if (ice > 0 && chance(mRandom) < 5.0 + (ice - 1) * 7.5) {
            int seconds = mConfig.iceDurationSeconds + (ice - 1) * 2;
            addEffect(victim, MobEffect::MOVEMENT_SLOWDOWN(), seconds * 20, 10, true);
            auto feet = victim.getFeetPos();
            auto& blockSource = victim.getDimensionBlockSource();
            int x = static_cast<int>(std::floor(feet.x));
            int y = static_cast<int>(std::floor(feet.y));
            int z = static_cast<int>(std::floor(feet.z));
            auto expires = mTick + static_cast<std::uint64_t>(seconds * 20);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = 0; dy <= 2; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        if (std::abs(dx) != 1 && std::abs(dz) != 1 && dy != 2) continue;
                        BlockPos pos(x + dx, y + dy, z + dz);
                        if (blockSource.getBlock(pos).isAir() && placeBlock(blockSource, pos, "minecraft:ice")) {
                            mTemporaryBlocks.push_back({&blockSource, pos, "minecraft:ice", expires});
                        }
                    }
                }
            }
        }
        int ghost = levelOf(enchants, "ghost");
        if (ghost > 0 && chance(mRandom) < 5.0 + (ghost - 1) * 5.0) {
            int seconds = 5 + (ghost - 1) * 3;
            addEffect(victim, MobEffect::MOVEMENT_SLOWDOWN(), seconds * 20, 255, true);
            addEffect(victim, MobEffect::JUMP(), seconds * 20, 128, false);
        }
        if (levelOf(enchants, "blood_demon") > 0 && chance(mRandom) < 20.0) {
            float drain = std::max(0.0f, victim.getHealth() * 0.25f);
            damage += drain;
            attacker.heal(static_cast<int>(std::ceil(drain)));
        }
    } else {
        if (levelOf(enchants, "volley") > 0 && chance(mRandom) < 10.0) damage *= 10.0f;
        if (levelOf(enchants, "web_bind") > 0 && chance(mRandom) < 10.0) {
            auto feet = victim.getFeetPos();
            auto& blockSource = victim.getDimensionBlockSource();
            int x = static_cast<int>(std::floor(feet.x));
            int y = static_cast<int>(std::floor(feet.y));
            int z = static_cast<int>(std::floor(feet.z));
            auto expires = mTick + static_cast<std::uint64_t>(mConfig.webDurationSeconds * 20);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = 0; dy <= 2; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        if (std::abs(dx) != 1 && std::abs(dz) != 1 && dy != 2) continue;
                        BlockPos pos(x + dx, y + dy, z + dz);
                        if (blockSource.getBlock(pos).isAir() && placeBlock(blockSource, pos, "minecraft:cobweb")) {
                            mTemporaryBlocks.push_back({&blockSource, pos, "minecraft:cobweb", expires});
                        }
                    }
                }
            }
        }
    }
}

void MyMod::openMainMenu(Player& player) {
    ll::form::SimpleForm form("§l§bEnchantForge", "§7请选择功能");
    form.appendButton("§b卷轴附魔", [this](Player& p) { playUiClick(p); openApplyScrollMenu(p); });
    form.appendButton("§c褪去卷轴附魔\n§7不返还卷轴", [this](Player& p) { playUiClick(p); openRemoveEnchantMenu(p); });
    form.appendButton("§d随机词条卷轴\n§7100000 LLMoney + 1 钻石块", [this](Player& p) {
        playUiClick(p);
        std::vector<double> weights;
        weights.reserve(mConfig.enchants.size());
        for (auto const& entry : mConfig.enchants) weights.push_back(std::max(0.0, entry.weight));
        if (weights.empty() || std::ranges::all_of(weights, [](double v) { return v <= 0.0; })) {
            p.sendMessage("§c随机词条池为空，请联系管理员");
            return;
        }
        auto diamondBlock = [](ItemStack const& item) {
            return !item.isNull() && item.getTypeName() == "minecraft:diamond_block";
        };
        if (countMatching(p, diamondBlock) < 1) {
            p.sendMessage("§c随机词条卷轴需要 1 个钻石块");
            return;
        }
        auto money = getLLMoneyApi();
        if (!money.available()) {
            p.sendMessage("§cLegacyMoney 未加载，暂时无法购买随机词条卷轴");
            return;
        }
        auto xuid = p.getXuid();
        if (money.get(xuid) < kRandomScrollPrice) {
            p.sendMessage("§c余额不足，随机词条卷轴需要 100000 LLMoney");
            return;
        }
        if (!money.reduce(xuid, kRandomScrollPrice)) {
            p.sendMessage("§cLLMoney 扣款失败，请稍后重试");
            return;
        }
        if (!removeMatching(p, 1, diamondBlock)) {
            if (money.add(xuid, kRandomScrollPrice)) p.sendMessage("§c钻石块扣除失败，100000 LLMoney 已自动退还");
            else p.sendMessage("§c钻石块扣除失败且自动退款失败，请联系管理员处理");
            return;
        }
        std::discrete_distribution<size_t> pick(weights.begin(), weights.end());
        auto const& definition = mConfig.enchants[pick(mRandom)];
        std::uniform_int_distribution<int> level(1, std::max(1, definition.maxLevel));
        auto scroll = makeScroll(definition, level(mRandom));
        p.addAndRefresh(scroll);
        playUiSuccess(p);
        p.sendMessage(fmt::format("§a花费 100000 LLMoney 和 1 个钻石块，获得 {} 卷轴", definition.name));
    });
    form.appendButton("§6合成突破卷轴", [this](Player& p) { playUiClick(p); openCraftMenu(p); });
    form.appendButton("§a使用突破卷轴", [this](Player& p) { playUiClick(p); openUpgradeMenu(p); });
    if (player.isOperator()) form.appendButton("§c管理员总表单", [this](Player& p) { playUiClick(p); openAdminMenu(p); });
    form.sendTo(player);
}

void MyMod::openRemoveEnchantMenu(Player& player) {
    auto& inventory = player.getInventory();
    auto item = inventory.getItemNonConst(player.getSelectedItemSlot());
    if (!item || item->isNull()) {
        player.sendMessage("§c请先手持要褪去附魔的物品");
        return;
    }
    auto enchants = readValidCustomEnchants(*item, mConfig.enchants);
    if (enchants.empty()) {
        player.sendMessage("§c手持物品没有可褪去的卷轴附魔");
        return;
    }
    struct Choice {
        std::string id;
        std::string name;
        int         level;
    };
    std::vector<Choice> choices;
    for (auto const& definition : mConfig.enchants) {
        int level = levelOf(enchants, definition.id);
        if (level > 0) choices.push_back({definition.id, definition.name, level});
    }
    ll::form::SimpleForm form("§l§c褪去卷轴附魔", "§7选择要从手持物品移除的扩展附魔\n§c操作不会返还卷轴");
    for (auto const& choice : choices) {
        form.appendButton(fmt::format("§c{} §f{}级", choice.name, choice.level), [this, choice](Player& p) {
            playUiClick(p);
            auto& inventory = p.getInventory();
            auto selectedSlot = p.getSelectedItemSlot();
            auto held = inventory.getItemNonConst(selectedSlot);
            if (!held || held->isNull()) {
                p.sendMessage("§c手持物品已变化，请重新打开菜单");
                return;
            }
            ItemStack updated(*held);
            auto current = readValidCustomEnchants(updated, mConfig.enchants);
            if (levelOf(current, choice.id) != choice.level || !removeCustomEnchant(updated, choice.id)) {
                p.sendMessage("§c物品附魔已变化，请重新打开菜单");
                return;
            }
            inventory.setItem(selectedSlot, updated);
            auto const& committed = inventory.getItem(selectedSlot);
            if (levelOf(readCustomEnchants(committed), choice.id) > 0) {
                p.sendMessage("§c附魔褪去写回失败，请重新尝试");
                return;
            }
            notifyInventory(p, selectedSlot);
            refreshCache(p, true);
            playUiSuccess(p);
            p.sendMessage(fmt::format("§a已褪去 {}，未返还卷轴", choice.name));
        });
    }
    form.sendTo(player);
}

void MyMod::openApplyScrollMenu(Player& player) {
    auto& inv = player.getInventory();
    auto target = inv.getItemNonConst(player.getSelectedItemSlot());
    if (!target || target->isNull()) {
        player.sendMessage("§c请先手持要附魔的物品");
        return;
    }
    std::vector<ScrollInfo> compatible;
    for (int slot = 0; slot < inv.getContainerSize(); ++slot) {
        auto parsed = readScroll(inv.getItem(slot));
        if (!parsed) continue;
        auto* definition = findDefinition(parsed->first);
        if (definition && matchesCategory(*target, definition->category)) compatible.push_back({slot, parsed->first, parsed->second});
    }
    if (compatible.empty()) {
        player.sendMessage("§c背包中没有适用于手持物品的卷轴");
        return;
    }
    ll::form::SimpleForm form("§l§b卷轴附魔", "§7选择要消耗的卷轴");
    for (auto const& scroll : compatible) {
        auto* definition = findDefinition(scroll.id);
        form.appendButton(fmt::format("§b{} §f{}级", definition->name, scroll.level), [this, scroll](Player& p) {
            playUiClick(p);
            auto& inventory = p.getInventory();
            auto targetItem = inventory.getItemNonConst(p.getSelectedItemSlot());
            if (!targetItem) return;
            auto currentScroll = readScroll(inventory.getItem(scroll.slot));
            auto* definition = findDefinition(scroll.id);
            if (!definition || !currentScroll || currentScroll->first != scroll.id || currentScroll->second != scroll.level
                || !matchesCategory(*targetItem, definition->category)) {
                p.sendMessage("§c物品或卷轴已变化，请重新打开菜单");
                return;
            }
            int old = levelOf(readValidCustomEnchants(*targetItem, mConfig.enchants), scroll.id);
            int next = std::min(definition->maxLevel, std::max(old, scroll.level));
            if (old >= next) {
                p.sendMessage("§c该附魔已达到卷轴可提供的等级");
                return;
            }
            applyCustomEnchant(*targetItem, *definition, next);
            removeOneAt(p, scroll.slot);
            notifyInventory(p, p.getSelectedItemSlot());
            refreshCache(p, true);
            playUiSuccess(p);
            p.sendMessage(fmt::format("§a已附加 {} {}级", definition->name, next));
        });
    }
    form.sendTo(player);
}

void MyMod::openCraftMenu(Player& player) {
    auto typeIs = [](std::string expected) {
        return [expected = std::move(expected)](ItemStack const& item) { return !item.isNull() && item.getTypeName() == expected; };
    };
    int maxCount = std::min({
        countMatching(player, typeIs("minecraft:glow_ink_sac")) / 5,
        countMatching(player, typeIs("minecraft:nether_star")),
        countMatching(player, typeIs("minecraft:conduit")),
        countMatching(player, typeIs("minecraft:netherite_block")),
        countMatching(player, typeIs("minecraft:dragon_head")),
        64,
    });
    if (maxCount <= 0) {
        player.sendMessage("§c材料不足：每个需要 5 荧光墨囊、1 下界之星、1 潮涌核心、1 下界合金块、1 龙首");
        return;
    }
    ll::form::CustomForm form("§l§6合成突破卷轴");
    form.appendSlider("count", "§e数量", 1, maxCount, 1, 1).setSubmitButton("§a合成");
    form.sendTo(player, [this](Player& p, ll::form::CustomFormResult const& result, auto) {
        if (!result || !result->contains("count")) return;
        playUiClick(p);
        auto countValue = readFormNumber(result->at("count"));
        if (!countValue || *countValue < 1.0 || *countValue > 64.0) {
            p.sendMessage("§c表单数量无效，请重新打开菜单");
            return;
        }
        int count = static_cast<int>(*countValue);
        auto typeIs = [](std::string expected) {
            return [expected = std::move(expected)](ItemStack const& item) { return !item.isNull() && item.getTypeName() == expected; };
        };
        if (countMatching(p, typeIs("minecraft:glow_ink_sac")) < count * 5
            || countMatching(p, typeIs("minecraft:nether_star")) < count
            || countMatching(p, typeIs("minecraft:conduit")) < count
            || countMatching(p, typeIs("minecraft:netherite_block")) < count
            || countMatching(p, typeIs("minecraft:dragon_head")) < count) {
            p.sendMessage("§c材料已变化，合成取消");
            return;
        }
        removeMatching(p, count * 5, typeIs("minecraft:glow_ink_sac"));
        removeMatching(p, count, typeIs("minecraft:nether_star"));
        removeMatching(p, count, typeIs("minecraft:conduit"));
        removeMatching(p, count, typeIs("minecraft:netherite_block"));
        removeMatching(p, count, typeIs("minecraft:dragon_head"));
        auto scroll = makeUpgradeScroll(count);
        p.addAndRefresh(scroll);
        playUiSuccess(p);
        p.sendMessage(fmt::format("§a成功合成 {} 个突破卷轴", count));
    });
}

void MyMod::openUpgradeMenu(Player& player) {
    auto& inv = player.getInventory();
    auto item = inv.getItemNonConst(player.getSelectedItemSlot());
    if (!item || item->isNull()) {
        player.sendMessage("§c请手持已有原版附魔的物品");
        return;
    }
    int scrollSlot = -1;
    for (int slot = 0; slot < inv.getContainerSize(); ++slot) if (isUpgradeScroll(inv.getItem(slot))) { scrollSlot = slot; break; }
    if (scrollSlot < 0) {
        player.sendMessage("§c背包中没有突破卷轴");
        return;
    }
    struct Choice { Enchant::Type type; std::string name; int level; };
    std::vector<Choice> choices;
    for (auto const& [type, name] : kVanillaEnchants) {
        int level = EnchantUtils::getEnchantLevel(type, *item);
        if (level > 0 && level < mConfig.maxVanillaBreakthrough) choices.push_back({type, std::string{name}, level});
    }
    if (choices.empty()) {
        player.sendMessage("§c手持物品没有可突破的原版附魔");
        return;
    }
    ll::form::SimpleForm form("§l§a原版附魔突破", fmt::format("§7最高 {} 级", mConfig.maxVanillaBreakthrough));
    for (auto const& choice : choices) {
        form.appendButton(fmt::format("§a{} §f{} → §e{}", choice.name, choice.level, choice.level + 1), [this, choice, scrollSlot](Player& p) {
            playUiClick(p);
            auto held = p.getInventory().getItemNonConst(p.getSelectedItemSlot());
            if (!held || !isUpgradeScroll(p.getInventory().getItem(scrollSlot))) return;
            int current = EnchantUtils::getEnchantLevel(choice.type, *held);
            if (current != choice.level || current >= mConfig.maxVanillaBreakthrough) {
                p.sendMessage("§c物品已变化，请重新打开菜单");
                return;
            }
            if (!setVanillaEnchantLevel(*held, choice.type, current + 1)) {
                p.sendMessage("§c附魔突破失败");
                return;
            }
            removeOneAt(p, scrollSlot);
            notifyInventory(p, p.getSelectedItemSlot());
            playUiSuccess(p);
            p.sendMessage(fmt::format("§a{} 已提升至 {} 级", choice.name, current + 1));
        });
    }
    form.sendTo(player);
}

void MyMod::openAdminMenu(Player& player) {
    if (!player.isOperator()) return;
    ll::form::SimpleForm form("§l§cEnchantForge 管理员", "§7管理手持物品和随机池");
    form.appendButton("§b手持物品：扩展附魔", [this](Player& p) { playUiClick(p); openAdminCustomEnchantMenu(p); });
    form.appendButton("§e手持物品：原版附魔", [this](Player& p) { playUiClick(p); openAdminVanillaEnchantMenu(p); });
    form.appendButton("§d编辑随机词条权重", [this](Player& p) { playUiClick(p); openAdminWeightMenu(p); });
    form.appendButton("§a重载配置", [this](Player& p) {
        playUiClick(p);
        loadConfig();
        playUiSuccess(p);
        p.sendMessage("§aEnchantForge 配置已重载");
    });
    form.sendTo(player);
}

void MyMod::openAdminCustomEnchantMenu(Player& player) {
    if (!player.isOperator()) return;
    auto const& held = player.getInventory().getItem(player.getSelectedItemSlot());
    if (held.isNull()) {
        player.sendMessage("§c请先手持要附魔的物品");
        return;
    }
    std::vector<std::string> names;
    std::vector<std::string> enchantIds;
    for (auto const& entry : mConfig.enchants) {
        if (!matchesCategory(held, entry.category)) continue;
        names.push_back(fmt::format("{}（最高 {} 级）", entry.name, entry.maxLevel));
        enchantIds.push_back(entry.id);
    }
    if (enchantIds.empty()) {
        player.sendMessage("§c当前手持物品没有可用的扩展附魔");
        return;
    }
    ll::form::CustomForm form("§l§b管理员扩展附魔");
    form.appendDropdown("enchant", "§b附魔", names)
        .appendInput("level", "§e等级", "超过最高等级时自动封顶", "1")
        .setSubmitButton("§a应用");
    form.sendTo(player, [this, names = std::move(names), enchantIds = std::move(enchantIds)](
                            Player& p, ll::form::CustomFormResult const& result, auto
                        ) {
        if (!result || !result->contains("enchant") || !result->contains("level")) return;
        playUiClick(p);
        auto selected = readFormIndex(result->at("enchant"), names);
        auto levelText = std::get_if<std::string>(&result->at("level"));
        if (!selected || *selected >= enchantIds.size() || !levelText) {
            p.sendMessage("§c表单数据无效，请重新打开菜单");
            return;
        }
        auto definition = findDefinition(enchantIds[*selected]);
        if (!definition) {
            p.sendMessage("§c扩展附魔配置已变化，请重新打开菜单");
            return;
        }
        int requestedLevel = 1;
        try { requestedLevel = std::max(1, std::stoi(*levelText)); }
        catch (...) { p.sendMessage("§c等级必须是正整数"); return; }
        auto item = p.getInventory().getItemNonConst(p.getSelectedItemSlot());
        if (!item || item->isNull()) { p.sendMessage("§c请手持物品"); return; }
        if (!matchesCategory(*item, definition->category)) {
            p.sendMessage("§c该扩展附魔不适用于当前手持物品");
            return;
        }
        int level = std::min(requestedLevel, std::max(1, definition->maxLevel));
        applyCustomEnchant(*item, *definition, level);
        notifyInventory(p, p.getSelectedItemSlot());
        refreshCache(p, true);
        playUiSuccess(p);
        if (level < requestedLevel) {
            p.sendMessage(fmt::format("§a{} 最高为 {} 级，已自动按 {} 级应用", definition->name, level, level));
        } else {
            p.sendMessage(fmt::format("§a已应用 {} {} 级", definition->name, level));
        }
    });
}

void MyMod::openAdminVanillaEnchantMenu(Player& player) {
    if (!player.isOperator()) return;
    std::vector<std::string> names;
    for (auto const& [type, name] : kVanillaEnchants) names.emplace_back(name);
    ll::form::CustomForm form("§l§e管理员原版附魔");
    form.appendDropdown("enchant", "§e附魔", names).appendInput("level", "§b等级", "任意正整数", "1").setSubmitButton("§a应用");
    form.sendTo(player, [names = std::move(names)](Player& p, ll::form::CustomFormResult const& result, auto) {
        if (!result || !result->contains("enchant") || !result->contains("level")) return;
        playUiClick(p);
        auto index = readFormIndex(result->at("enchant"), names);
        auto levelText = std::get_if<std::string>(&result->at("level"));
        if (!index || *index >= kVanillaEnchants.size() || !levelText) {
            p.sendMessage("§c表单数据无效，请重新打开菜单");
            return;
        }
        int level = 1;
        try { level = std::clamp(std::stoi(*levelText), 1, 32767); }
        catch (...) { p.sendMessage("§c等级必须是正整数"); return; }
        auto item = p.getInventory().getItemNonConst(p.getSelectedItemSlot());
        if (!item || item->isNull()) { p.sendMessage("§c请手持物品"); return; }
        if (!setVanillaEnchantLevel(*item, kVanillaEnchants[*index].first, level)) {
            p.sendMessage("§c原版附魔应用失败");
            return;
        }
        notifyInventory(p, p.getSelectedItemSlot());
        playUiSuccess(p);
        p.sendMessage("§a原版附魔已应用");
    });
}

void MyMod::openAdminWeightMenu(Player& player) {
    if (!player.isOperator()) return;
    std::vector<std::string> names;
    for (auto const& entry : mConfig.enchants) names.push_back(fmt::format("{} - 当前 {}", entry.name, entry.weight));
    ll::form::CustomForm form("§l§d编辑随机词条权重");
    form.appendDropdown("enchant", "§d词条", names).appendInput("weight", "§e权重", "0 表示不抽取", "1").setSubmitButton("§a保存");
    form.sendTo(player, [this, names = std::move(names)](Player& p, ll::form::CustomFormResult const& result, auto) {
        if (!result || !result->contains("enchant") || !result->contains("weight")) return;
        playUiClick(p);
        auto index = readFormIndex(result->at("enchant"), names);
        auto weightText = std::get_if<std::string>(&result->at("weight"));
        if (!index || *index >= mConfig.enchants.size() || !weightText) {
            p.sendMessage("§c表单数据无效，请重新打开菜单");
            return;
        }
        double weight = 0;
        try { weight = std::max(0.0, std::stod(*weightText)); }
        catch (...) { p.sendMessage("§c权重必须是非负数字"); return; }
        mConfig.enchants[*index].weight = weight;
        ll::config::saveConfig(mConfig, getSelf().getConfigDir() / "config.json");
        playUiSuccess(p);
        p.sendMessage("§a随机权重已保存");
    });
}

} // namespace enchant_forge

LL_REGISTER_MOD(enchant_forge::MyMod, enchant_forge::MyMod::getInstance());
