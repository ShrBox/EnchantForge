# Changelog

## 1.3.0 - 2026-11-06

- Ported to BDS 26.51 / LeviLamina 26.51.9.
- Bumped the build dependency, the `tooth.json` dependency and both CI workflows to 26.51.9. The manifest schema and the `plugins/EnchantForge/` install path are unchanged.
- Re-checked every public LeviLamina API this mod uses (events, forms, commands, config, mod system) against the 26.51.9 headers: no source changes were required.
- Added the `_HAS_CXX23=1` define to match the 26.51 mod template.
- Enchantment behaviour and the config format are unchanged, so an existing `config.json` needs no migration.

## 1.2.5

- Changed Life Boost to use a near-permanent effect duration while its chestplate is equipped.
- The effect is restored if externally cleared, updates when the enchantment level changes, and is removed immediately after unequipping.

## 1.2.4

- Fixed removing the final custom enchant by using Bedrock's dedicated `clearCustomLore()` API when no lore remains.
- Applied the same empty-lore handling to revive-use metadata so stale markers cannot survive a clear operation.

## 1.2.3

- Fixed the last remaining custom enchant sometimes failing to be removed from an item.
- Custom-enchant removal now edits a copy, verifies the marker is gone, and commits the complete item back to its inventory slot.

## 1.2.2

- Fixed Skyward sometimes losing `MayFly` after login equipment and ability synchronization, which prevented double-jump flight.
- The mod now checks the real server-side `MayFly` permission and only restores it when missing; it still never forces `Flying` while Skyward is equipped.

## 1.2.1

- Fixed Skyward repeatedly resending `MayFly`, which caused movement corrections while falling or transitioning in and out of Elytra gliding.
- Skyward now behaves like vanilla creative flight: it grants `MayFly` once, leaves the `Flying` state to the player's double-jump input, and only resyncs once after respawn.

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-08-10

- Added 识忆、冷启动、鬼手、柴刀、鬼域、屹立不倒 and 无量空处.
- Fixed night vision flicker and removal after unequipping the helmet.
- Fixed vanilla breakthrough failing when updating an enchantment already on an item.
- Added config migration so existing installations receive all new enchant definitions.
