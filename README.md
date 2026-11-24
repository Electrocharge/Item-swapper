# Item Swapper

A native LeviLauncher mod for Minecraft Bedrock on Android (arm64-v8a).

## Behaviour

- Hold a sword and press **SWAP**: selects the first axe in the hotbar.
- Hold an axe and press **SWAP**: selects the first sword in the hotbar.
- Hold anything else, or have no matching tool in the hotbar: nothing happens.
- Only the selected hotbar index changes. Item stacks are never moved, copied, or overwritten.

## Build

GitHub Actions builds the Android arm64 package and uploads a `.levipack` artifact.

Target version is Minecraft Bedrock 1.26.62.x / LeviLauncher 1.7.1. The update-hook signature and inventory layout are based on BedrockTools SDK references; the selected-slot member offset is cross-checked against independent Bedrock SDK sources but still needs an in-game test on the target build.

## Important test note

This is the first implementation build, not a device-verified release. If the game build's update signature does not match, the mod logs an unsupported-build message and declines to install the hook rather than guessing at a function address.
