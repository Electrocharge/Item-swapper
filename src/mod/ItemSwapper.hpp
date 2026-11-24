#pragma once

#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace item_swapper {

class ItemSwapperMod {
public:
    static ItemSwapperMod& instance();
    ItemSwapperMod();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod& mSelf;
    pl::memory::HookHandle mClientUpdateHook;
};

}
