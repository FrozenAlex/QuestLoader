#pragma once

#include <stdlib.h>
#include <vector>
#include <string>
#include <string_view>

// Returns the libil2cpp.so path, should only be called AFTER mods have been constructed
// Check Mod::constructed for validity
std::string getLibIl2CppPath();

// Provides metadata for each mod
class Mod
{
    public:
        static std::vector<Mod> mods;
        static bool constructed;
        Mod(std::string_view name_, std::string_view path, void *handle_) : name(name_), pathName(path), handle(handle_) {}
        bool loaded;
        std::string name;
        std::string pathName;
        void init();
        void load();
    private:
        void *handle;
        bool init_loaded;
        void (*init_func)(void);
        bool load_loaded;
        void (*load_func)(void);
};