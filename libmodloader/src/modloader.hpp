#pragma once

#include <stdlib.h>
#include <vector>
#include <string>
#include <string_view>

// Returns the libil2cpp.so path, should only be called AFTER mods have been constructed
// Check Mod::constructed for validity
std::string getLibIl2CppPath();

typedef struct ModInfo {
    std::string id;
    std::string version;
} ModInfo;

// Provides metadata for each mod
class Mod
{
    public:
        static std::vector<Mod> mods;
        static bool constructed;
        Mod(std::string_view name_, std::string_view path, ModInfo info_, void *handle_) : name(name_), pathName(path), info(info_), handle(handle_) {}
        const std::string name;
        const std::string pathName;
        const ModInfo info;
        bool get_loaded();
        void init_mod();
        void load_mod();
    private:
        bool loaded;
        void *handle;
        bool init_loaded = false;
        void (*init_func)(void) = NULL;
        bool load_loaded = false;
        void (*load_func)(void) = NULL;
};