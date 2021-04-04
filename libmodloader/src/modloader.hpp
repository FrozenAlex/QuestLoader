#pragma once

#include <stdlib.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>

struct ModInfo {
    std::string id;
    std::string version;
};

struct ModloaderInfo {
    std::string name;
    std::string tag;
};

// C API for nice rust linkage
extern "C" const char* get_info_id(ModInfo* instance);
extern "C" void set_info_id(ModInfo* instance, const char* name);
extern "C" const char* get_info_version(ModInfo* instance);
extern "C" void set_info_version(ModInfo* instance, const char* version);
extern "C" const char* get_modloader_name(ModloaderInfo* instance);
extern "C" const char* get_modloader_tag(ModloaderInfo* instance);

class Mod;

// Different copy of Modloader for the modloader itself vs. as a header
#ifndef MODLOADER_DEFINED
class Modloader {
    public:
        // Returns the libil2cpp.so path, should only be called AFTER mods have been constructed
        // Check Modloader::getAllConstructed() for validity
        static const std::string getLibIl2CppPath();
        // Returns the application id, should only be called AFTER mods have been constructed
        // Check Modloader::getAllConstructed() for validity
        // Example return: com.beatgames.beatsaber
        static const std::string getApplicationId();
        // Returns whether all mods on this modloader have been loaded or not
        static bool getAllConstructed();
        // Modloader info
        static const ModloaderInfo getInfo();
        // A map of id to mods managed by this modloader
        static const std::unordered_map<std::string, const Mod> getMods();
        // Require another mod to be loaded, should only be called AFTER mods have been constructed
        // Will block until the required mod is loaded, if it exists.
        // If it does not exist, or this was called before mod loading was complete, returns immediately.
        static bool requireMod(const ModInfo&);
        // Same as requireMod(const ModInfo&) except uses an ID and a version string
        static bool requireMod(std::string_view id, std::string_view version);
        // Require another mod to be loaded, should only be called AFTER mods have been constructed
        // Will block until all versions of the specified id are loaded, if any exist.
        // If none exist, or if this was called before mod loading was complete (in the case of a recursive load) returns immediately.
        static bool requireMod(std::string_view id);
};
#endif

// Provides metadata for each mod
class Mod {
    friend class Modloader;
    public:
        Mod(std::string_view name_, std::string_view path, ModInfo info_, void *handle_) : name(name_), pathName(path), info(info_), handle(handle_) {}
        const std::string name;
        const std::string pathName;
        const ModInfo info;
        bool get_loaded() const;
        bool operator==(const Mod& m) const {
            return info.id == m.info.id && info.version == m.info.version;
        }
    private:
        void init_mod();
        void load_mod();
        bool loaded = false;
        void *handle = nullptr;
        bool init_loaded = false;
        void (*init_func)(void) = NULL;
        bool load_loaded = false;
        void (*load_func)(void) = NULL;
};

namespace std {
    template<>
    struct hash<Mod> {
        size_t operator()(const Mod& m) const {
            return std::hash<std::string>{}(m.info.id) ^ (std::hash<std::string>{}(m.info.version) << 1);
        }
    };
}