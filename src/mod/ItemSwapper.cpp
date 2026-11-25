#include "ItemSwapper.hpp"

#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <pl/memory/Signature.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace item_swapper {
namespace {

constexpr char kModuleId[] = "item_swapper";
constexpr char kButtonId[] = "item_swapper_swap_button";
constexpr char kGameLibrary[] = "libminecraftpe.so";

// BedrockTools' ClientInstanceUpdate signature and SDK vtable indices.
// The update hook runs the queued request after vanilla's update on the game thread.
constexpr char kClientInstanceUpdateSignature[] =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 "
    "? ? ? 91 ? ? ? D1 59 D0 3B D5 ? ? ? 52 F5 03 02 2A";

constexpr std::size_t kClientGetLocalPlayerVtable = 31;
constexpr std::size_t kPlayerGetCarriedItemVtable = 78;
constexpr std::size_t kItemGetDescriptionIdVtable = 5;

enum class SwapMechanic : int { SwordToAxe = 0, AxeToPickaxe = 1, SwordToPickaxe = 2, MaceKit = 3 };

// BedrockTools Inventory.hpp layout: Player + 0x590 -> PlayerInventory proxy;
// proxy + 0xB8 -> inventory container; container + 0x140 -> item pointer range.
// PlayerInventory::selectedSlot is +0x10 in independently published Bedrock SDKs.
// ItemStack objects are 0x98 bytes; the first nine are the hotbar.
constexpr std::size_t kPlayerInventoryOffset = 0x590;
constexpr std::size_t kSelectedSlotOffset = 0x10;
constexpr std::size_t kPlayerInventoryContainerOffset = 0xB8;
constexpr std::size_t kFillingContainerItemsOffset = 0x140;
constexpr std::size_t kItemStackSize = 0x98;
constexpr std::size_t kItemStackItemCounterOffset = 0x08;
constexpr std::size_t kItemStackCountOffset = 0x22;
constexpr std::size_t kItemStackValidOffset = 0x23;

using ClientInstanceUpdateFn = void* (*)(void*, bool);

std::atomic<bool> gEnabled{false};
std::atomic<int> gSwapMechanic{static_cast<int>(SwapMechanic::SwordToAxe)};
std::atomic<bool> gSwordSwap{false};
std::atomic<bool> gAxeSwap{false};
std::atomic<bool> gMaceSkipStep{false};
// Remember the last Mace Kit selection so equipping a chestplate (which removes
// it from the selected hotbar slot) still advances to the mace on the next press.
std::atomic<int> gMaceLastStep{-1};
std::atomic<int> gMaceLastSlot{-1};
std::atomic<bool> gSwapRequested{false};
ClientInstanceUpdateFn gClientInstanceUpdateOriginal = nullptr;

bool isReadableStack(void* stack) {
    if (!stack) return false;
    auto* bytes = static_cast<std::byte*>(stack);
    if (*reinterpret_cast<std::uint8_t*>(bytes + kItemStackValidOffset) == 0) return false;
    if (*reinterpret_cast<std::uint8_t*>(bytes + kItemStackCountOffset) == 0) return false;
    void* counter = *reinterpret_cast<void**>(bytes + kItemStackItemCounterOffset);
    return counter && *reinterpret_cast<void**>(counter);
}

const std::string* itemDescription(void* stack) {
    if (!isReadableStack(stack)) return nullptr;

    auto* bytes = static_cast<std::byte*>(stack);
    void* counter = *reinterpret_cast<void**>(bytes + kItemStackItemCounterOffset);
    void* item = *reinterpret_cast<void**>(counter);
    if (!item) return nullptr;

    auto** vtable = *reinterpret_cast<void***>(item);
    if (!vtable || !vtable[kItemGetDescriptionIdVtable]) return nullptr;

    using GetDescriptionIdFn = const std::string& (*)(const void*);
    const auto& description =
        reinterpret_cast<GetDescriptionIdFn>(vtable[kItemGetDescriptionIdVtable])(item);
    if (description.empty() || description.size() > 128) return nullptr;
    return &description;
}

enum class ToolKind { Other, Sword, Axe, Pickaxe, Elytra, Fireworks, Chestplate, Mace };

ToolKind classify(void* stack) {
    const auto* description = itemDescription(stack);
    if (!description) return ToolKind::Other;
    if (description->find("elytra") != std::string::npos) return ToolKind::Elytra;
    if (description->find("fireworks") != std::string::npos || description->find("firework_rocket") != std::string::npos) return ToolKind::Fireworks;
    if (description->find("chestplate") != std::string::npos) return ToolKind::Chestplate;
    if (description->find("mace") != std::string::npos) return ToolKind::Mace;
    if (description->find("sword") != std::string::npos) return ToolKind::Sword;
    if (description->find("_axe") != std::string::npos) return ToolKind::Axe;
    if (description->find("pickaxe") != std::string::npos) return ToolKind::Pickaxe;
    return ToolKind::Other;
}

void* getLocalPlayer(void* client) {
    if (!client) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(client);
    if (!vtable || !vtable[kClientGetLocalPlayerVtable]) return nullptr;
    using GetLocalPlayerFn = void* (*)(void*);
    return reinterpret_cast<GetLocalPlayerFn>(vtable[kClientGetLocalPlayerVtable])(client);
}

void* getCarriedItem(void* player) {
    if (!player) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(player);
    if (!vtable || !vtable[kPlayerGetCarriedItemVtable]) return nullptr;
    using GetCarriedItemFn = void* (*)(void*);
    return reinterpret_cast<GetCarriedItemFn>(vtable[kPlayerGetCarriedItemVtable])(player);
}

bool findHotbarTarget(void* player, ToolKind wanted, int currentSlot, int& targetSlot) {
    auto* playerBytes = static_cast<std::byte*>(player);
    void* inventoryProxy =
        *reinterpret_cast<void**>(playerBytes + kPlayerInventoryOffset);
    if (!inventoryProxy) return false;

    auto* proxyBytes = static_cast<std::byte*>(inventoryProxy);
    void* inventory =
        *reinterpret_cast<void**>(proxyBytes + kPlayerInventoryContainerOffset);
    if (!inventory) return false;

    auto* inventoryBytes = static_cast<std::byte*>(inventory);
    const auto begin = *reinterpret_cast<std::uintptr_t*>(
        inventoryBytes + kFillingContainerItemsOffset);
    const auto end = *reinterpret_cast<std::uintptr_t*>(
        inventoryBytes + kFillingContainerItemsOffset + sizeof(void*));

    if (!begin || end < begin || (end - begin) % kItemStackSize != 0) return false;
    const auto count = (end - begin) / kItemStackSize;
    if (count < 9 || count > 64) return false;

    for (int slot = 0; slot < 9; ++slot) {
        if (slot == currentSlot) continue;
        auto* stack = reinterpret_cast<void*>(begin + static_cast<std::size_t>(slot) * kItemStackSize);
        if (classify(stack) == wanted) {
            targetSlot = slot;
            return true;
        }
    }
    return false;
}

void performSwap(void* client) {
    if (!gEnabled.load(std::memory_order_acquire) ||
        !gSwapRequested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    void* player = getLocalPlayer(client);
    if (!player) return;

    void* carried = getCarriedItem(player);
    const ToolKind heldKind = classify(carried);
    const auto mechanic = static_cast<SwapMechanic>(
        gSwapMechanic.load(std::memory_order_acquire));

    ToolKind source = ToolKind::Sword;
    ToolKind target = ToolKind::Axe;
    bool fallbackEnabled = gSwordSwap.load(std::memory_order_acquire);

    switch (mechanic) {
        case SwapMechanic::SwordToAxe:
            source = ToolKind::Sword;
            target = ToolKind::Axe;
            fallbackEnabled = gSwordSwap.load(std::memory_order_acquire);
            break;
        case SwapMechanic::AxeToPickaxe:
            source = ToolKind::Axe;
            target = ToolKind::Pickaxe;
            fallbackEnabled = gAxeSwap.load(std::memory_order_acquire);
            break;
        case SwapMechanic::SwordToPickaxe:
            source = ToolKind::Sword;
            target = ToolKind::Pickaxe;
            fallbackEnabled = gSwordSwap.load(std::memory_order_acquire);
            break;
        case SwapMechanic::MaceKit: {
            constexpr ToolKind kitOrder[] = {
                ToolKind::Elytra, ToolKind::Fireworks, ToolKind::Chestplate, ToolKind::Mace
            };
            auto* playerBytes = static_cast<std::byte*>(player);
            void* inventoryProxy = *reinterpret_cast<void**>(playerBytes + kPlayerInventoryOffset);
            if (!inventoryProxy) return;
            auto* selectedSlot = reinterpret_cast<int*>(static_cast<std::byte*>(inventoryProxy) + kSelectedSlotOffset);
            const int currentSlot = *selectedSlot;
            if (currentSlot < 0 || currentSlot > 8) return;

            int nextIndex = 0;
            if (heldKind == ToolKind::Elytra) nextIndex = 1;
            else if (heldKind == ToolKind::Fireworks) nextIndex = 2;
            else if (heldKind == ToolKind::Chestplate) nextIndex = 3;
            else if (heldKind == ToolKind::Mace) nextIndex = 0;
            else {
                // Equipping a chestplate can empty the selected hotbar slot.
                // If that is the slot we just selected as the chestplate, advance
                // to mace rather than restarting the kit at Elytra/fireworks.
                const int lastStep = gMaceLastStep.load(std::memory_order_acquire);
                const int lastSlot = gMaceLastSlot.load(std::memory_order_acquire);
                if (lastStep == 2 && lastSlot == currentSlot) nextIndex = 3;
            }

            const bool skipStep = gMaceSkipStep.load(std::memory_order_acquire);
            const int attempts = skipStep ? 4 : 1;
            for (int step = 0; step < attempts; ++step) {
                const int wantedIndex = (nextIndex + step) % 4;
                const ToolKind wanted = kitOrder[wantedIndex];
                int targetSlot = -1;
                if (findHotbarTarget(player, wanted, currentSlot, targetSlot)) {
                    *selectedSlot = targetSlot;
                    gMaceLastStep.store(wantedIndex, std::memory_order_release);
                    gMaceLastSlot.store(targetSlot, std::memory_order_release);
                    return;
                }
            }
            return;
        }
    }

    ToolKind wanted = ToolKind::Other;
    if (heldKind == source) {
        wanted = target;
    } else if (heldKind == target) {
        wanted = source;
    } else if (fallbackEnabled) {
        wanted = source;
    } else {
        return;
    }

    auto* playerBytes = static_cast<std::byte*>(player);
    void* inventoryProxy =
        *reinterpret_cast<void**>(playerBytes + kPlayerInventoryOffset);
    if (!inventoryProxy) return;

    auto* selectedSlot = reinterpret_cast<int*>(
        static_cast<std::byte*>(inventoryProxy) + kSelectedSlotOffset);
    const int currentSlot = *selectedSlot;
    if (currentSlot < 0 || currentSlot > 8) return;

    int targetSlot = -1;
    if (!findHotbarTarget(player, wanted, currentSlot, targetSlot)) return;

    // Change only the selected hotbar index. No ItemStack is copied or modified.
    *selectedSlot = targetSlot;
}

void* clientInstanceUpdateDetour(void* client, bool value) {
    void* result = gClientInstanceUpdateOriginal
        ? gClientInstanceUpdateOriginal(client, value)
        : nullptr;
    performSwap(client);
    return result;
}

} // namespace

ItemSwapperMod& ItemSwapperMod::instance() {
    static ItemSwapperMod mod;
    return mod;
}

ItemSwapperMod::ItemSwapperMod()
    : mSelf(*ll::mod::NativeMod::current()) {}

bool ItemSwapperMod::load() {
    return true;
}

bool ItemSwapperMod::enable() {
    auto& logger = getSelf().getLogger();

    const auto target = pl::memory::resolveSignature(
        kClientInstanceUpdateSignature, kGameLibrary);
    if (!target) {
        logger.error("Item Swapper: ClientInstanceUpdate signature was not found; game build may be unsupported");
        return false;
    }

    mClientUpdateHook = pl::memory::HookHandle(
        reinterpret_cast<pl::memory::FuncPtr>(target),
        reinterpret_cast<pl::memory::FuncPtr>(&clientInstanceUpdateDetour),
        reinterpret_cast<pl::memory::FuncPtr*>(&gClientInstanceUpdateOriginal));
    if (!mClientUpdateHook.installed()) {
        logger.error("Item Swapper: failed to install the game-update hook");
        return false;
    }

    const bool moduleRegistered = pl::modmenu::ModuleBuilder(kModuleId, "Item Swapper")
        .description("Swap between selected tool types in your hotbar.")
        .modId(getSelf().getId())
        .defaultEnabled(true)
        .onToggle([](std::string_view, bool enabled) {
            gEnabled.store(enabled, std::memory_order_release);
        })
        .onConfigChanged([](std::string_view, std::string_view key, std::string_view value) {
            if (key == "swap_mechanic") {
                int mechanic = static_cast<int>(SwapMechanic::SwordToAxe);
                if (value == "axe_to_pickaxe") mechanic = static_cast<int>(SwapMechanic::AxeToPickaxe);
                else if (value == "sword_to_pickaxe") mechanic = static_cast<int>(SwapMechanic::SwordToPickaxe);
                else if (value == "mace_kit") mechanic = static_cast<int>(SwapMechanic::MaceKit);
                gSwapMechanic.store(mechanic, std::memory_order_release);
            } else if (key == "sword_swap") {
                gSwordSwap.store(value == "true" || value == "1", std::memory_order_release);
            } else if (key == "axe_swap") {
                gAxeSwap.store(value == "true" || value == "1", std::memory_order_release);
            } else if (key == "mace_skip_step") {
                gMaceSkipStep.store(value == "true" || value == "1", std::memory_order_release);
            }
        })
        .registerModule();

    if (moduleRegistered) {
        using namespace pl::modmenu;
        ConfigSchemaBuilder schema;
        schema.defaultCategory("general").category("general", "General");
        
        ConfigNodeV2 mechanic;
        mechanic.id = "swap_mechanic";
        mechanic.key = "swap_mechanic";
        mechanic.category = "general";
        mechanic.section = "Swap mechanics";
        mechanic.title = "Swap mechanic";
        mechanic.type = ConfigControlTypeV2::Choice;
        mechanic.choiceStyle = ConfigChoiceStyleV2::Radio;
        mechanic.defaultValue = "sword_to_axe";
        mechanic.options = {
            {"sword_to_axe", "Sword to axe"},
            {"axe_to_pickaxe", "Axe to pickaxe"},
            {"sword_to_pickaxe", "Sword to pickaxe"},
            {"mace_kit", "Mace Kit"}
        };
        schema.node(std::move(mechanic));

        ConfigNodeV2 swordFallback;
        swordFallback.id = "sword_swap";
        swordFallback.key = "sword_swap";
        swordFallback.category = "general";
        swordFallback.section = "Swap mechanics";
        swordFallback.title = "Sword Swap";
        swordFallback.type = ConfigControlTypeV2::Toggle;
        swordFallback.defaultValue = "false";
        swordFallback.visibleWhen = {
            {"swap_mechanic", ConfigConditionOpV2::NotEquals, "axe_to_pickaxe"},
            {"swap_mechanic", ConfigConditionOpV2::NotEquals, "mace_kit"}
        };
        schema.node(std::move(swordFallback));

        ConfigNodeV2 axeFallback;
        axeFallback.id = "axe_swap";
        axeFallback.key = "axe_swap";
        axeFallback.category = "general";
        axeFallback.section = "Swap mechanics";
        axeFallback.title = "Axe Swap";
        axeFallback.type = ConfigControlTypeV2::Toggle;
        axeFallback.defaultValue = "false";
        axeFallback.visibleWhen = {
            {"swap_mechanic", ConfigConditionOpV2::Equals, "axe_to_pickaxe"}
        };
        schema.node(std::move(axeFallback));

        ConfigNodeV2 maceSkipStep;
        maceSkipStep.id = "mace_skip_step";
        maceSkipStep.key = "mace_skip_step";
        maceSkipStep.category = "general";
        maceSkipStep.section = "Swap mechanics";
        maceSkipStep.title = "Skip step";
        maceSkipStep.type = ConfigControlTypeV2::Toggle;
        maceSkipStep.defaultValue = "false";
        maceSkipStep.visibleWhen = {
            {"swap_mechanic", ConfigConditionOpV2::Equals, "mace_kit"}
        };
        schema.node(std::move(maceSkipStep));

        if (!setConfigSchemaJson(kModuleId, schema.toJson())) {
            logger.warn("Item Swapper: Mod Menu rejected the Swap mechanics settings schema");
        }
    }

    const bool buttonRegistered = pl::modmenu::ButtonBuilder(kButtonId, "Item Swapper")
        .moduleId(kModuleId)
        .modId(getSelf().getId())
        .label("SWAP")
        .behavior(pl::modmenu::ButtonBehavior::Click)
        .defaultVisible(true)
        .onEvent([](std::string_view, pl::modmenu::ButtonEvent event, float) {
            if (event == pl::modmenu::ButtonEvent::Click &&
                gEnabled.load(std::memory_order_acquire)) {
                gSwapRequested.store(true, std::memory_order_release);
            }
        })
        .registerButton();

    if (!moduleRegistered || !buttonRegistered) {
        logger.error("Item Swapper: failed to register its Mod Menu controls");
        mClientUpdateHook.reset();
        pl::modmenu::unregisterButton(kButtonId);
        pl::modmenu::unregisterModule(kModuleId);
        return false;
    }

    gEnabled.store(true, std::memory_order_release);
    logger.info("Item Swapper loaded; selectable hotbar swap mechanics are ready");
    return true;
}

bool ItemSwapperMod::disable() {
    gEnabled.store(false, std::memory_order_release);
    gSwapRequested.store(false, std::memory_order_release);
    mClientUpdateHook.reset();
    pl::modmenu::unregisterButton(kButtonId);
    pl::modmenu::unregisterModule(kModuleId);
    return true;
}

bool ItemSwapperMod::unload() {
    return disable();
}

}
