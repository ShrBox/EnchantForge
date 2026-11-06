# EnchantForge → BDS 26.51 / LeviLamina 26.51.9 移植报告

版本：1.2.5（适配 LeviLamina 26.10.14）→ 1.3.0（适配 LeviLamina 26.51.9）
日期：2026-11-06

## TL;DR

- 上游原版（`sunxiaochuan2231/EnchantForge@1.2.5`）只适配到 **LeviLamina 26.10.14**，仓库里**没有** 26.51 的构建产物。
- 本次把工程锁到 **LeviLamina 26.51.9**（对应 Minecraft 26.51.1 / BDS 包名 `1.26.51`），改了 4 类文件：`xmake.lua`、`tooth.json`、`.github/workflows/*.yml`、文档版本号。
- **业务代码 `src/mod/MyMod.cpp` 一行没改**：我逐个核对了它用到的全部 LeviLamina 公有 API，26.51.9 的头文件里签名全部对得上（见下节）。
- **没编译过**。本机没有 MSVC/xmake，且 GitHub 直连不通。剩下唯一的不确定项是 **`mc/` 层（BDS 26.51.1 的引擎头文件）**，那部分拿不到、只能在你的机器或 CI 上编译验证。

## 改了什么

| 文件 | 改动 |
| --- | --- |
| `xmake.lua` | `set_version("1.3.0")`；`add_requires("levilamina 26.51.9")`；`modpacker modVersion = "1.3.0"`；新增 `_HAS_CXX23=1` |
| `tooth.json` | `version` → `1.3.0`；依赖 `LeviLamina` → `26.51.9`；描述文案跟着改；安装路径仍是 `plugins/EnchantForge/` |
| `.github/workflows/build.yml`、`release.yml` | `xrepo install ... "levilamina 26.51.9"` |
| `README.md` | 版本行、安装说明、构建说明（补了 clang-cl 备选） |
| `CHANGELOG.md` | 新增 1.3.0 条目 |
| `docs/ENCHANTMENTS.md` | 「对应 1.2.5 默认配置」→ `1.3.0`（机制本身没动） |
| `manifest.json` | **未改**：26.51.9 的 `Manifest` 结构体仍是 `entry/name/type/platform/version`，字段完全兼容 |

## 核对过的 API（对照 LeviLamina 26.51.9 真实头文件）

| 本模组用法 | 26.51.9 里的样子 | 结论 |
| --- | --- | --- |
| `ll::mod::NativeMod::current()` + `*` 解引用 | `static std::shared_ptr<NativeMod> current(HandleT = ...)`，**与 26.10.14 完全相同** | 不用改 |
| `LL_REGISTER_MOD(CLAZZ, BINDER)` | 宏形状不变，内部走 `bindToMod()` + `onLoad/onEnable/onDisable/onUnload` 回调 | 不用改（`MyMod::load/enable/disable` 满足 Loadable/Enableable/Disableable 概念） |
| `EventBus::emplaceListener<T>(fn, EventPriority::High)` | `emplaceListener<T>(Args&&...)` 转发给 `Listener<T>::create(fn, priority)` | 不用改 |
| `EventPriority` | **仍在**，但从 `ll/api/event/EventPriority.h` 挪到了 `ll/api/event/ListenerBase.h` | 本模组没有直接 include 旧头，不受影响；自己写代码时要改 include |
| `ActorHurtEvent::self()/source()/damage()/cancel()` | 一致（`damage()` 仍是 `float&`） | 不用改 |
| `PlayerUseItemEvent::item()/cancel()` | 一致（`ItemStack&`） | 不用改 |
| `PlayerJoin/Respawn/Disconnect/DestroyBlock`、`ServerLevelTickEvent` | 一致 | 不用改 |
| `SimpleForm::appendButton/sendTo`、`CustomForm::appendDropdown/appendInput/appendSlider/setSubmitButton/sendTo` | 签名一致；`CustomFormResult` 仍是 `optional<unordered_map<string, variant<monostate,uint64,double,string>>>` | 不用改 |
| `CommandRegistrar::getOrCreateCommand(name, desc, perm)` / `executeCommand(cmd, origin, CommandOutputType::Silent)` | 一致；`executeCommand` 现在返回 `CommandOutput`（原来是 `void`，忽略返回值即可） | 不用改 |
| `ll::config::loadConfig/saveConfig` | 一致；`IsConfig` 仍要求 `version` 是整型，`PluginConfig.version` 满足 | 不用改 |
| 模组目录 | 服务端仍是 `plugins/`（PreLoader 找不到时回退 `mods/`，`modsPath` 可覆盖） | `tooth.json` 的 placements 不用改 |

## 26.10.14 → 26.51.9 之间，与本模组相关的 LeviLamina 变更

摘自上游 `CHANGELOG.md`：

- **26.51.0**：适配 Minecraft 26.51.1；移除 `ProtocolCompatibility`；**entt 升到 v4.0.0**（事件系统底层，`ll::event` 封装层未变）。
- **26.51.2**：删除 `ll::io::DefaultSink`；`ll::io::FileSink` 改为接收 `RotatePolicy`；`logs/latest.log` 改为按大小/日期轮转。本模组不直接用 IO。
- **26.51.4**：新增 `NetherNetPatch`，默认单端口（部署相关，不是代码兼容问题）。
- **26.51.7**：**reflection 重写**、移除 `ll::io::Pipe`、`PreLoader` 升到 1.16.5。本模组没直接用 reflection（只经 `ll::config` 间接用到）。
- **26.51.9**：`bedrock-runtime-data` 版本跟进（`26.51.1-server.12`）。

一句话：这一波改动主要在日志、网络、反射、底层容器，避开了本模组依赖的那条「事件 → 表单 → 命令 → 配置」主干。

## 我验证不了的部分（重要）

`mc/` 命名空间是 BDS 引擎侧的头文件，不在 LeviLamina 仓库里（来自 `bedrock-runtime-data`，只发运行时数据、不发头文件），我这边拿不到。本模组深度依赖它，以下调用**只能在编译时确认**：

- `ItemStack`：`getCustomLore/setCustomLore/clearCustomLore`、`setCustomName(Bedrock::Safety::RedactableString)`
- `EnchantUtils::getEnchantLevel/applyEnchant`、`ItemEnchants`/`EnchantmentInstance`、`constructItemEnchantsFromUserData/saveEnchantsToUserData`
- `Enchant::Type` 枚举完整性（模组里硬编码了 42 项，含 `Lunge/突刺`、`Density`、`Breach`、`WindBurst`）
- `MobEffectInstance` 的 `mDuration->mValue` / `mAmplifier` / `mAmbient` / `mEffectVisible`，`MobEffect::XXX()`
- `AbilitiesIndex::MayFly`、`Player::canUseAbility/setAbility`、`ArmorSlot`、`hurtByCause`、`BlockType::popResource`、`BlockSource::setBlockAndRetainCompatibleBlockActor`、`ServerCommandOrigin`

这类差异绝大多数是**编译期报错**（不是静默行为错误），所以「编不过」就是最好的探测器。少数静默风险（枚举值顺序、数值语义）需要跑服验证。

## 怎么验证

### A. 本地编译

需要：Visual Studio 2022 C++ 生成工具 + ATL + Windows SDK、xmake、能访问 GitHub（`xmake-repo` 要从 GitHub 拉包，防火墙内先设镜像）：

```powershell
xmake repo --add -g levimc-repo https://github.com/LiteLDev/xmake-repo.git
xrepo install -y -j1 -p windows -a x64 -m release -f "target_type=server" "levilamina 26.51.9"
xrepo install -y -j1 -p windows -a x64 -m release levibuildscript
xmake f -y -m release --target_type=server
xmake -y -j1
```

产物在 `bin/EnchantForge/`。

### B. 交给 CI（推荐，最省事）

把这份工程推到你自己的 fork，`build.yml` 会在 PR/push 时用 `windows-latest` 编一次，产物在 Actions 的 artifact 里；打个 `v1.3.0` release 会触发 `release.yml` 自动打包成 `EnchantForge-server-windows-x64.zip` 并附到 release —— 也正是 `tooth.json` 期望的文件名。

## 编不过时的排查顺序

1. **报错在 `mc/` 里的符号找不到** → BDS 26.51.1 改了内部 API，按报错点上游 `bedrock-runtime-data@26.51.1-server.12` 对应的头文件改名/改签名。
2. **报错在 `ll/api/...`** → 可能是 `_HAS_CXX23=1` 或工具链问题：把 `set_toolchains("msvc")` 换成 `set_toolchains("clang-cl")`（26.51 官方模板已改用 clang-cl）。
3. **`Enchant::Type::XXX` 不存在** → 26.51 的附魔枚举增删，删掉对应条目即可（`kVanillaEnchants` 表在 `MyMod.cpp` 里是纯数据，很安全）。
4. **链接期缺符号** → `xrepo` 缓存里的 levilamina 版本不干净，`xmake clean -a` 后重装 26.51.9。

## 附录：只是想把服务器上的 LL 升到 26.51

如果你的目的其实是升级服务端加载器（而不是改插件），流程是：

```powershell
# 1. 停服并整目录备份（mods/plugins、worlds、bedrock_server.exe 都要）
# 2. 在服务器目录
lip install github.com/LiteLDev/bds@1.26.51        # 或手动替换 bedrock_server.exe
lip update github.com/LiteLDev/LeviLamina           # 拉到 26.51.9
lip update                                          # 其余依赖（PreLoader 1.16.5 等）
# 3. 启动 bedrock_server_mod.exe
```

注意：
- `lip update` 会保留 `plugins/EnchantForge/config/config.json`（`tooth.json` 里声明了 `preserve_files`），但插件本体还是 26.10 编的**旧 DLL，加载会失败** —— 必须换成按 26.51.9 重编的版本。
- 挂防火墙时先 `lip config set github_proxy https://v4.gh-proxy.org/https://github.com/`。
- 26.51.4 起 `NetherNetPatch` 默认单端口，若你有端口映射/联机插件，升级后确认 `plugins/LeviLamina/config/Config.json` 里的 `netherNetPatch` 段。
