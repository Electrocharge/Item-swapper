# Item Swapper

A native LeviLauncher mod for Minecraft Bedrock on Android (arm64-v8a).

## Swap mechanics

Choose exactly one mechanic in the **Swap mechanics** section:

- **Sword to axe** (default): sword ↔ axe.
- **Axe to pickaxe**: axe ↔ pickaxe.
- **Sword to pickaxe**: sword ↔ pickaxe.
- **Mace Kit**: Elytra → Fireworks → any chestplate in the hotbar → Mace → Elytra.

The selector is single-choice, so choosing one mechanic automatically replaces the previous choice.

With **Mace Kit** selected, each press of SWAP advances to the next item in that sequence. If you're holding something outside the kit, it starts by selecting an Elytra. If the next item isn't present in the hotbar, the swap does nothing by default.

## Fallback settings

- **Sword Swap** appears for **Sword to axe** and **Sword to pickaxe**. When enabled, pressing SWAP while holding neither item in the selected pair selects a sword.
- **Axe Swap** appears only for **Axe to pickaxe**. When enabled, pressing SWAP while holding neither item in that pair selects an axe.
- **Skip step** appears only for **Mace Kit**. When enabled, if the next item in the sequence isn't in the hotbar, it continues forward and selects the next available kit item instead of doing nothing.

All fallback settings default to OFF. If the requested item is not in the hotbar, nothing happens (unless **Skip step** is enabled for Mace Kit).

Only the selected hotbar index changes. Item stacks are never moved, copied, or overwritten.

## Build

GitHub Actions builds the Android arm64 package and uploads a `.levipack` artifact.

Target version is Minecraft Bedrock 1.26.62.x / LeviLauncher 1.7.1. The update-hook signature and inventory layout are based on BedrockTools SDK references; the selected-slot member offset is cross-checked against independent Bedrock SDK sources but still needs an in-game test on the target build.

## Important test note

This implementation still needs a fresh build and in-game test on the target device. If the game build's update signature does not match, the mod logs an unsupported-build message and declines to install the hook rather than guessing at a function address.
