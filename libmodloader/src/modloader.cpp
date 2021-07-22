// Only define the Modloader class here, instead of getting it from the header
#define MODLOADER_DEFINED
#include <libmain.hpp>
#include <modloader.hpp>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <linux/limits.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <jni.h>
// #include "jit/jit.hpp"
#include "log.hpp"

#include <sys/mman.h>
#include <dlfcn.h>
#include "../../beatsaber-hook/shared/utils/utils.h"
#include <libgen.h>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include "protection.hpp"

#undef TAG
#define TAG "libmodloader"

#define MOD_PATH_FMT "/sdcard/Android/data/%s/files/mods/"
#define LIBS_PATH_FMT "/sdcard/Android/data/%s/files/libs/"

// There should only be ONE modloader PER GAME
// Ideally, there is only ONE modloader per libmodloader.so
class Modloader {
    public:
        static const std::string getLibIl2CppPath();
        static const std::string getApplicationId();
        static bool getAllConstructed();
        static const ModloaderInfo getInfo();
        static const std::unordered_map<std::string, const Mod> getMods();
        static bool requireMod(const ModInfo&);
        static bool requireMod(std::string_view id, std::string_view version);
        static bool requireMod(std::string_view id);
        static const std::string getModloaderPath();
        static const std::string getDestinationPath();
        static JNIEnv* getJni();
        // New members, specific to .cpp only        
        static void init_mods() noexcept;
        static void load_mods() noexcept;
        static bool allConstructed;
        static std::string modloaderPath;
        static std::string modPath;
        static std::string libsPath;
        static std::string modTempPath;
        static std::string applicationId;
        static std::string libIl2CppPath;
        static void construct_mods() noexcept;
        static void setInfo(ModloaderInfo& info);
    private:
        static bool setDataDirs();
        static ModloaderInfo info;
        static std::unordered_map<std::string, Mod> mods;
        static std::unordered_set<Mod> loadingMods;
        static void copy_to_temp(std::string path, const char* filename);
        static bool copy(std::string_view pathToCopy);
        static bool try_load_libs();
        static bool try_setup_mods();
        static bool try_load_recurse(std::vector<std::pair<std::string, std::string>>& failed, bool (*attempt_load)(std::string first, const char* second));
        static bool lib_loader(std::string first, const char* second);
        static void* construct_mod(const char* filename);
        static bool create_mod(std::string modPath, const char* name);
        static void setup_mod(void *handle, ModInfo& modInfo);
};

bool Modloader::allConstructed;
std::string Modloader::modloaderPath;
std::string Modloader::modPath;
std::string Modloader::libsPath;
std::string Modloader::modTempPath;
std::string Modloader::applicationId;
std::string Modloader::libIl2CppPath;
ModloaderInfo Modloader::info;
std::unordered_map<std::string, Mod> Modloader::mods;
std::unordered_set<Mod> Modloader::loadingMods;
static JNIEnv* modloaderEnv;

// Generic utility functions
#pragma region Generic Utilities

static jobject getActivityFromUnityPlayerInternal(JNIEnv *env) {
    jclass clazz = env->FindClass("com/unity3d/player/UnityPlayer");
    if (clazz == NULL) return nullptr;
    jfieldID actField = env->GetStaticFieldID(clazz, "currentActivity", "Landroid/app/Activity;");
    if (actField == NULL) return nullptr;
    return env->GetStaticObjectField(clazz, actField);
}

static jobject getActivityFromUnityPlayer(JNIEnv *env) {
    jobject activity = getActivityFromUnityPlayerInternal(env);
    if (activity == NULL) {
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        logpf(ANDROID_LOG_ERROR, "libmain.getActivityFromUnityPlayer failed! See 'System.err' tag.");
        env->ExceptionClear();
    }
    return activity;
}

static bool ensurePermsInternal(JNIEnv* env, jobject activity) {
    jclass clazz = env->FindClass("com/unity3d/player/UnityPlayerActivity");
    if (clazz == NULL) return false;
    jmethodID checkSelfPermission = env->GetMethodID(clazz, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (checkSelfPermission == NULL) return false;
    const jstring perm = env->NewStringUTF("android.permission.WRITE_EXTERNAL_STORAGE");
    jint hasPerm = env->CallIntMethod(activity, checkSelfPermission, perm);
    logpf(ANDROID_LOG_DEBUG, "checkSelfPermission(WRITE_EXTERNAL_STORAGE) returned: %i", hasPerm);
    if (hasPerm != 0) {
        jmethodID requestPermissions = env->GetMethodID(clazz, "requestPermissions", "([Ljava/lang/String;I)V");
        if (requestPermissions == NULL) return false;
        jclass stringClass = env->FindClass("java/lang/String");
        jobjectArray arr = env->NewObjectArray(1, stringClass, perm);
        jint requestCode = 21326;  // the number in the alphabet for each letter in BMBF (B=2, M=13, F=6)
        logpf(ANDROID_LOG_INFO, "Calling requestPermissions!");
        env->CallVoidMethod(activity, requestPermissions, arr, requestCode);
        if (env->ExceptionCheck()) return false;
    }
    return true;
}

#define NULLOPT_UNLESS(expr, ...) ({ \
auto&& __tmp = (expr); \
if (!__tmp) {logpf(ANDROID_LOG_WARN, __VA_ARGS__); return std::nullopt;} \
__tmp; })

static std::optional<jstring> getDestination(JNIEnv* env) {
    auto activityThreadClass = NULLOPT_UNLESS(env->FindClass("android/app/ActivityThread"), "Failed to find android.app.ActivityThread!");
    auto currentActivityThreadMethod = NULLOPT_UNLESS(env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;"), "Failed to find ActivityThread.currentActivityThread");
    auto contextClass = NULLOPT_UNLESS(env->FindClass("android/content/Context"), "Failed to find android.context.Context!");
    auto getApplicationMethod = NULLOPT_UNLESS(env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;"), "Failed to find Application.getApplication");
    auto filesDirMethod = NULLOPT_UNLESS(env->GetMethodID(contextClass, "getFilesDir", "()Ljava/io/File;"), "Failed to find Context.getFilesDir()!");
    auto fileClass = NULLOPT_UNLESS(env->FindClass("java/io/File"), "Failed to find java.io.File!");
    auto absDirMethod = NULLOPT_UNLESS(env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;"), "Failed to find File.getAbsolutePath()!");

    auto at = NULLOPT_UNLESS(env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod), "Returned result from currentActivityThread is null!");
    auto context = NULLOPT_UNLESS(env->CallObjectMethod(at, getApplicationMethod), "Returned result from getApplication is null!");
    auto file = NULLOPT_UNLESS(env->CallObjectMethod(context, filesDirMethod), "Returned result from getFilesDir is null!");
    auto str = NULLOPT_UNLESS(env->CallObjectMethod(file, absDirMethod), "Returned result from getAbsolutePath is null!");

    return reinterpret_cast<jstring>(str);
}

static bool ensurePerms(JNIEnv* env, jobject activity) {
    if (ensurePermsInternal(env, activity)) return true;

    if (env->ExceptionCheck()) env->ExceptionDescribe();
    logpf(ANDROID_LOG_ERROR, "libmain.ensurePerms failed! See 'System.err' tag.");
    env->ExceptionClear();
    return false;
}

char *trimWhitespace(char *str)
{
  char *end;
  while(isspace((unsigned char)*str)) str++;
  if(*str == 0)
    return str;

  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;

  end[1] = '\0';

  return str;
}

int mkpath(std::string stringPath, mode_t mode) {
    // Pass a copy of the string to mkpath
    char* file_path = stringPath.data();
    for (char* p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(file_path, mode) == -1) {
            if (errno != EEXIST) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }
    return 0;
}
#pragma endregion

// Modloader functions
#pragma region Modloader Functions

const std::string Modloader::getModloaderPath() {
    return modloaderPath;
}

const std::string Modloader::getDestinationPath() {
    return modTempPath;
}

JNIEnv* Modloader::getJni() {
    return modloaderEnv;
}

// MUST BE CALLED BEFORE LOADING MODS
bool Modloader::setDataDirs()
{
    FILE *cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
        //not sure what the actual max is, but path_max should cover it
        char application_id[PATH_MAX] = {0};
        fread(application_id, sizeof(application_id), 1, cmdline);
        fclose(cmdline);
        trimWhitespace(application_id);
        applicationId = application_id;
        modPath = string_format(MOD_PATH_FMT, application_id);
        libsPath = string_format(LIBS_PATH_FMT, application_id);
        auto res = getDestination(modloaderEnv);
        if (res) {
            auto str = modloaderEnv->GetStringUTFChars(*res, nullptr);
            modTempPath = str;
            modTempPath.push_back('/');
            logpfm(ANDROID_LOG_INFO, "Destination path: %s", modTempPath.c_str());
            // string is copied, so free the allocated version
            modloaderEnv->ReleaseStringUTFChars(*res, str);
        } else {
            logpfm(ANDROID_LOG_WARN, "Could not obtain data path from JNI! Falling back to /data/data instead.");
            modTempPath = string_format("/data/data/%s/", application_id);
        }
        system((std::string("mkdir -p -m +rwx ") + modTempPath).c_str());
        return true;
    } else {
        return false;
    }
}

void Modloader::copy_to_temp(std::string path, const char* filename) {
    auto full_path = path + filename;
    logpfm(ANDROID_LOG_INFO, "Copying file: %s", full_path.c_str());
    int infile = open(full_path.c_str(), O_RDONLY);
    off_t filesize = lseek(infile, 0, SEEK_END);
    lseek(infile, 0, SEEK_SET);

    logpfm(ANDROID_LOG_VERBOSE, "Temp path: %s", modTempPath.c_str());
    std::string temp_path = modTempPath + filename;
    logpfm(ANDROID_LOG_VERBOSE, "Local full path: %s", temp_path.c_str());

    int outfile = open(temp_path.c_str(), O_CREAT | O_WRONLY, 0777);
    sendfile(outfile, infile, 0, filesize);
    close(infile);
    close(outfile);
    chmod(temp_path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
}

// TODO Find a way to avoid calling constructor on mods that have offsetless hooks in constructor
// Loads the mod at the given full_path
// Returns the dlopened handle
void* Modloader::construct_mod(const char* filename) {
    // Calls the constructor on the mod by loading it
    // Copying should have already taken place.
    std::string temp_path(modTempPath);
    temp_path.append(filename);
    auto *ret = dlopen(temp_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    protect();
    if (ret == NULL) {
        // Error logging (for if symbols cannot be resolved)
        auto s = dlerror();
        logpfm(ANDROID_LOG_WARN, "dlerror when dlopening: %s: %s", temp_path.c_str(), s == NULL ? "null" : s);
    }
    return ret;
}

// Calls the setup(ModInfo&) function on the mod, if it exists
// This will be immediately after mod construction
void Modloader::setup_mod(void *handle, ModInfo& modInfo) {
    logpfm(ANDROID_LOG_VERBOSE, "Setting up mod handle: %p", handle);
    void (*setup_func)(ModInfo&);
    *(void**)(&setup_func) = dlsym(handle, "setup");
    logpfm(ANDROID_LOG_VERBOSE, "Found setup function: %p", setup_func);
    if (setup_func) {
        // We don't need to pass in a Modloader pointer because we have one in static anyways!
        setup_func(modInfo);
    }
}

// Returns true if a mod was successfully created, false otherwise.
bool Modloader::create_mod(std::string modPath, const char* name) {
    auto *modHandle = construct_mod(name);
    if (modHandle == NULL) {
        return false;
    }
    ModInfo modInfo;
    setup_mod(modHandle, modInfo);
    if (modInfo.id.empty()) {
        // Fallback to library name if it doesn't have an id
        modInfo.id = name;
    }
    if (modInfo.version.empty()) {
        // Fallback to 0.0.0 if it doesn't have a version
        modInfo.version = "0.0.0";
    }
    // Don't overwrite existing mod IDs, warn when doing so
    if (!mods.try_emplace(modInfo.id, name, modPath, modInfo, modHandle).second) {
        logpfm(ANDROID_LOG_WARN, "Could not construct mod with name: %s, id: %s, version: %s because another mod of that same id exists!", name, modInfo.id.c_str(), modInfo.version.c_str());
    }
    logpfm(ANDROID_LOG_INFO, "Created mod with name: %s, id: %s, version: %s, path: %s, handle: %p", name, modInfo.id.c_str(), modInfo.version.c_str(), modPath.c_str(), modHandle);
    return true;
}

// Copies all .so files from pathToCopy to the temp directory
bool Modloader::copy(std::string_view pathToCopy) {
    logpfm(ANDROID_LOG_VERBOSE, "Copying all .so files from: %s to temp directory: %s", pathToCopy.data(), modTempPath.c_str());
    struct dirent* dp;
    DIR *dir = opendir(pathToCopy.data());
    if (dir == NULL) {
        logpfm(ANDROID_LOG_ERROR, "copy(%s): null dir! errno: %i, msg: %s", pathToCopy.data(), errno, strerror(errno));
        // We can actually continue, but without copying over any of the libraries
        return false;
    } else {
        while ((dp = readdir(dir)) != NULL) {
            if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so")) {
                // We want to copy all .so files to our temp path, or we can dlopen them locally
                copy_to_temp(pathToCopy.data(), dp->d_name);
            }
        }
        closedir(dir);
    }
    return true;
}

/// @brief Attempts to recursively load failed .so files from the provided list.
/// Modifies the vector and calls attempt_load on each potentially loadable mod/lib
bool Modloader::try_load_recurse(std::vector<std::pair<std::string, std::string>>& failed, bool (*attempt_load)(std::string first, const char* second)) {
    if (failed.size() > 0) {
        auto oldSize = failed.size() + 1;
        // While the new failed size is less than the old size, continue to try to load
        // If we reach a point where we cannot load any mods (deadlock) we will have equivalent oldSize and failed.size()
        while (failed.size() < oldSize && failed.size() != 0) {
            std::vector<std::pair<std::string, std::string>> tempFailed;
            logpfm(ANDROID_LOG_WARN, "Failed List:");
            for (const auto& item : failed) {
                auto str = item.first + item.second;
                logpfm(ANDROID_LOG_INFO, "Previously failed: %s Trying again...", str.c_str());
                if (!attempt_load(item.first, item.second.c_str())) {
                    // If we failed to open it again, we add it to the temporary list.
                    logpfm(ANDROID_LOG_ERROR, "STILL Failed to dlopen: %s", str.c_str());
                    tempFailed.emplace_back(item.first, item.second);
                } else {
                    logpfm(ANDROID_LOG_INFO, "Successfully loaded: %s", str.c_str());
                }
            }
            oldSize = failed.size();
            failed.clear();
            failed = tempFailed;
#ifdef __aarch64__
            logpfm(ANDROID_LOG_VERBOSE, "After completing pass, had: %lu failed, now have: %lu", oldSize, failed.size());
#else
            logpfm(ANDROID_LOG_VERBOSE, "After completing pass, had: %u failed, now have: %u", oldSize, failed.size());
#endif
        }
        return failed.size() == 0;
    }
    return true;
}

bool Modloader::lib_loader(std::string name, const char* second) {
    auto str = name + second;
    auto* tmp = dlopen(str.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (tmp == NULL) {
        auto s = dlerror();
        logpfm(ANDROID_LOG_ERROR, "Failed to dlopen: %s, dlerror: %s", str.c_str(), s == nullptr ? "NULL" : s);
        return false;
    }
    return true;
}

// Responsible for loading libraries
// Returns true on success (all libs loaded) false otherwise
bool Modloader::try_load_libs() {
    logpfm(ANDROID_LOG_INFO, "Loading all libs!");
    // Failed to load libs
    std::vector<std::pair<std::string, std::string>> failed;
    
    // Try to open libs
    struct dirent* dp;
    DIR* dir = opendir(libsPath.c_str());
    if (dir == nullptr) {
        logpfm(ANDROID_LOG_ERROR, "Not opening libs %s: null dir! errno: %i, msg: %s!", libsPath.c_str(), errno, strerror(errno));
        return false;
    } else {
        while ((dp = readdir(dir)) != NULL) {
            // Iterate over the directory AGAIN
            // This time this happens after all mods are copied so we can ensure proper linkage
            // dlopen each one
            if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so")) {
                auto str = modTempPath + dp->d_name;
                auto* tmp = dlopen(str.c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (tmp == NULL) {
                    auto s = dlerror();
                    logpfm(ANDROID_LOG_ERROR, "Failed to dlopen: %s, dlerror: %s", str.c_str(), s == nullptr ? "NULL" : s);
                    failed.emplace_back(modTempPath, dp->d_name);
                } else {
                    logpfm(ANDROID_LOG_INFO, "Successfully loaded lib: %s", str.c_str());
                }
                // We shouldn't need to keep this handle anywhere, we can just throw it away without closing it.
                // This should hopefully force the library to stay open
            }
        }
        closedir(dir);
    }

    // List failed libs and try again
    // What we want to do here is while we haven't changed size of failed, we continue trying to load from failed.
    return try_load_recurse(failed, lib_loader);
}

// Responsible for setting up mods
// Returns true on success (all mods loaded) false otherwise
bool Modloader::try_setup_mods() {
    logpfm(ANDROID_LOG_INFO, "Constructing all mods!");
    // Failed mods
    std::vector<std::pair<std::string, std::string>> failed;
    // Iterate over mods and attempt to construct them
    struct dirent* dp;
    DIR* dir = opendir(modPath.c_str());
    if (dir == nullptr) {
        logpfm(ANDROID_LOG_FATAL, "construct_mods(%s): %s: null dir! errno: %i, msg: %s", modloaderPath.data(), modPath.c_str(), errno, strerror(errno));
        return false;
    } else {
        while ((dp = readdir(dir)) != NULL) {
            if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so")) {
                if (!create_mod(modPath, dp->d_name)) {
                    // We create it with modPath, because create_mod checks the temp dir itself.
                    // We want to make sure we create the mod with the correct path.
                    failed.emplace_back(modPath, dp->d_name);
                }
            }
        }
        closedir(dir);
    }
    return try_load_recurse(failed, Modloader::create_mod);
}

void Modloader::construct_mods() noexcept {
    libIl2CppPath = modloaderPath + "/libil2cpp.so";
    logpfm(ANDROID_LOG_DEBUG, "libil2cpp path: %s", libIl2CppPath.c_str());
    // Protect at least once on startup
    protect();
    // Open ourselves early to potentially fix some issues
    dlopen(NULL, RTLD_NOW|RTLD_GLOBAL);
    logpfm(ANDROID_LOG_DEBUG, "Constructing mods from modloader path: '%s'", modloaderPath.c_str());
    bool modReady = true;
    if (!setDataDirs())
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to determine data directories.");
        modReady = false;
    }
    else if (mkpath(modPath, 0) != 0)
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create mod path at '%s'", modPath.c_str());
        modReady = false;
    }
    else if (mkpath(libsPath, 0) != 0) 
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create library path at: '%s'", libsPath.c_str());
        modReady = false;
    }
    else if (mkpath(modTempPath, 0) != 0)
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create mod temporary path at '%s'", modTempPath.c_str());
        modReady = false;
    }
    if (!modReady) {
        logpfm(ANDROID_LOG_ERROR, "QuestHook failed to initialize, mods will not load.");
        return;
    }

    // We need to clear out all .so files from our /data/data directory.
    // This happens because we want to make sure we don't take up ever increasing storage.
    struct dirent* dp;
    DIR* dir = opendir(modTempPath.c_str());
    if (dir == nullptr) {
        logpfm(ANDROID_LOG_ERROR, "Could not clear temp dir %s: null dir! errno: %i, msg: %s!", modTempPath.c_str(), errno, strerror(errno));
        return;
    } else {
        while ((dp = readdir(dir)) != NULL) {
            if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so")) {
                auto str = modTempPath + dp->d_name;
                // Delete all .so files in our modTempPath
                if (unlink(str.c_str())) {
                    logpfm(ANDROID_LOG_WARN, "Failed to delete: %s errno: %i, msg: %s", str.c_str(), errno, strerror(errno));
                } else {
                    logpfm(ANDROID_LOG_VERBOSE, "Deleted: %s", str.c_str());
                }
            }
        }
        closedir(dir);
    }

    // Copy all mods and libs ahead of time, before any resolution occurs.
    bool success = copy(libsPath);
    if (!success) {
        logpfm(ANDROID_LOG_WARN, "One or more libs failed to copy! Continuing anyways...");
    }
    success = copy(modPath);
    if (!success) {
        logpfm(ANDROID_LOG_WARN, "One or more mods failed to copy! Continuing anyways...");
        return;
    }
    // Then, load all libs.
    // If we fail to load any, continue anyways.
    success = try_load_libs();
    if (!success) {
        logpfm(ANDROID_LOG_WARN, "One or more libs failed to load! Continuing anyways...");
    }
    success = try_setup_mods();
    if (!success) {
        logpfm(ANDROID_LOG_WARN, "One or more mods failed to be setup! Continuing anyways...");
    }

    Modloader::allConstructed = true;
    logpfm(ANDROID_LOG_INFO, "Done constructing mods!");
}

static void* imagehandle;
static void (*il2cppInit)(const char* domain_name);
// Loads the mods after il2cpp has been initialized
// Does not have to be offsetless since it is installed directly
MAKE_HOOK(il2cppInitHook, NULL, void, const char* domain_name)
{
    il2cppInitHook(domain_name);
    protect();
    Modloader::load_mods();
}

// Calls the init functions on all constructed mods
void Modloader::init_mods() noexcept {
    if (!allConstructed) {
        logpfm(ANDROID_LOG_ERROR, "Tried to initalize mods, but they are not yet constructed!");
        return;
    }
    logpfm(ANDROID_LOG_INFO, "Initializing all mods!");

    for (auto& mod : mods) {
        mod.second.init_mod();
    }

    logpfm(ANDROID_LOG_INFO, "Initialized all mods!");
    logpfm(ANDROID_LOG_VERBOSE, "dlopening libil2cpp.so: %s", libIl2CppPath.c_str());

    imagehandle = dlopen(libIl2CppPath.c_str(), RTLD_LOCAL | RTLD_LAZY);
    // On startup, we also want to protect everything, and ensure we have read/write
    protect();
    if (imagehandle == NULL) {
        logpfm(ANDROID_LOG_FATAL, "Could not dlopen libil2cpp.so! Not calling load on mods!");
        return;
    }
    *(void**)(&il2cppInit) = dlsym(imagehandle, "il2cpp_init");
	logpfm(ANDROID_LOG_INFO, "Loaded: il2cpp_init (%p)", il2cppInit);
    if (il2cppInit) {
        INSTALL_HOOK_DIRECT(il2cppInitHook, il2cppInit);
    } else {
        logpfm(ANDROID_LOG_ERROR, "Failed to dlsym il2cpp_init!");
    }
}

// Calls the load functions on all constructed mods
void Modloader::load_mods() noexcept {
    if (!Modloader::allConstructed) {
        logpfm(ANDROID_LOG_ERROR, "Tried to load mods, but they are not yet constructed!");
        return;
    }
    logpfm(ANDROID_LOG_INFO, "Loading all mods!");

    for (auto& mod : mods) {
        // Add each mod to the loadingMods list immediately before so it is not double loaded
        if (!mod.second.get_loaded()) {
            loadingMods.insert(mod.second);
            mod.second.load_mod();
        } else {
            logpfm(ANDROID_LOG_VERBOSE, "Mod: %s (id: %s) already loaded! Not loading again.", mod.first.c_str(), mod.second.info.id.c_str());
        }
    }

    logpfm(ANDROID_LOG_INFO, "Loaded all mods!");
}

// Returns the libil2cpp.so path
const std::string Modloader::getLibIl2CppPath() {
    return libIl2CppPath;
}

// Returns the application ID
const std::string Modloader::getApplicationId() {
    return applicationId;
}

// Returns whether all mods have been constructed or not
bool Modloader::getAllConstructed() {
    return allConstructed;
}

const std::unordered_map<std::string, const Mod> Modloader::getMods() {
    std::unordered_map<std::string, const Mod> temp;
    for (auto& m : mods) {
        temp.try_emplace(m.first, m.second);
    }
    return temp;
}

const ModloaderInfo Modloader::getInfo() {
    return info;
}

void Modloader::setInfo(ModloaderInfo& info) {
    Modloader::info = info;
}

bool Modloader::requireMod(std::string_view id) {
    if (!allConstructed) {
        // Do nothing if not all mods are constructed
        return false;
    }
    logpfm(ANDROID_LOG_VERBOSE, "Requiring mod: %s", id.data());
    auto m = mods.find(id.data());
    if (m != mods.end()) {
        logpfm(ANDROID_LOG_VERBOSE, "Found matching mod!");
        auto loading = loadingMods.find(m->second);
        if (loading != loadingMods.end()) {
            // If the mod is in our loadingMods, return early
            logpfm(ANDROID_LOG_VERBOSE, "Mod already in loadingMods (loading or is loaded!)");
            return true;
        }
        if (!m->second.get_loaded()) {
            // If the mod isn't already loaded, load it.
            logpfm(ANDROID_LOG_VERBOSE, "Loading mod...");
            loadingMods.insert(m->second);
            m->second.load_mod();
        }
        return true;
    }
    return false;
}
bool Modloader::requireMod(const ModInfo& info) {
    return Modloader::requireMod(info.id, info.version);
}
bool Modloader::requireMod(std::string_view id, std::string_view version) {
    // Find the matching mod in our list of constructed mods
    // If it doesn't exist, exit immediately.
    // If we find that a mod that is being required requires a mod that requires us, we have deadlock
    // So, in such a case, we would like to simply return immediately if we detect that this is the case.
    // loadingMods is a vector of all mods that are being loaded at the moment.
    // If the mod that we are attempting to require is already in this list, we return immediately.
    // Otherwise, we invoke the mod.load function on that mod and let it run to completion.
    if (!allConstructed) {
        // Do nothing if not all mods are constructed
        return false;
    }
    logpfm(ANDROID_LOG_VERBOSE, "Requiring mod: %s", id.data());
    auto m = mods.find(id.data());
    if (m != mods.end()) {
        logpfm(ANDROID_LOG_VERBOSE, "Found matching mod!");
        // Ensure version matches (a version match should be specific to the version for now)
        // Eventually, this should check if there exists a version >= the provided one.
        // TODO: ^
        if (m->second.info.version != version) {
            logpfm(ANDROID_LOG_VERBOSE, "Version mismatch: desired: %s, actual: %s", version.data(), m->second.info.version.c_str());
            return false;
        }
        auto loading = loadingMods.find(m->second);
        if (loading != loadingMods.end()) {
            // If the mod is in our loadingMods, return early
            logpfm(ANDROID_LOG_VERBOSE, "Mod already in loadingMods (loading or is loaded!)");
            return true;
        }
        if (!m->second.get_loaded()) {
            // If the mod isn't already loaded, load it.
            logpfm(ANDROID_LOG_VERBOSE, "Loading mod...");
            loadingMods.insert(m->second);
            m->second.load_mod();
        }
        return true;
    }
    return false;
}
#pragma endregion

// Mod functionality
#pragma region Mod Functions
bool Mod::get_loaded() const {
    return loaded;
}

// Calls the init() function on the mod, if it exists
// This will be before il2cpp functionality is available
// Called in preload
void Mod::init_mod() {
    logpf(ANDROID_LOG_INFO, "Initializing mod: %s, id: %s, version: %s, handle: %p", pathName.c_str(), info.id.c_str(), info.version.c_str(), handle);
    if (!init_loaded) {
        *(void**)(&init_func) = dlsym(handle, "init");
        init_loaded = true;
    }
    logpf(ANDROID_LOG_VERBOSE, "Found init function: %p", init_func);
    if (init_loaded && init_func) {
        Dl_info info;
        dladdr((void *)init_func, &info);
        logpf(ANDROID_LOG_VERBOSE, "dladdr of init function base: %p name: %s", info.dli_fbase, info.dli_sname);
        init_func();
    }
}

// Calls the load() function on the mod, if it exists
// This will be after il2cpp functionality is available
// Called immediately after il2cpp_init
void Mod::load_mod() {
    logpf(ANDROID_LOG_INFO, "Loading mod: %s, id: %s, version: %s", pathName.c_str(), info.id.c_str(), info.version.c_str());
    if (!load_loaded) {
        *(void**)(&load_func) = dlsym(handle, "load");
        load_loaded = true;
    }
    if (load_loaded && load_func) {
        load_func();
    }
    loaded = true;
}
#pragma endregion

static void init_all_mods() {
    Modloader::init_mods();
}

extern "C" void modloader_preload() noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_preload called (should be really early)");
    logpf(ANDROID_LOG_INFO, "Welcome!");
}

extern "C" JNINativeInterface modloader_main(JavaVM* v, JNIEnv* env, std::string_view loadSrc) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_main called with vm: 0x%p, env: 0x%p, loadSrc: %s", v, env, loadSrc.data());

    jobject activity = getActivityFromUnityPlayer(env);
    if (activity) ensurePerms(env, activity);

    auto iface = jni::interface::make_passthrough_interface<JNINativeInterface>(&env->functions);

    // Create libil2cpp path string. Should be in the same path as loadSrc (since libmodloader.so needs to be in the same path)
    char *dirPath = dirname(loadSrc.data());
    if (dirPath == NULL) {
        logpf(ANDROID_LOG_FATAL, "loadSrc cannot be converted to a valid directory!");
        return iface;
    }
    // TODO: Check if path exists before setting it and assuming it is valid
    ModloaderInfo info;
    info.name = "MainModloader";
    info.tag = "main-modloader";
    modloaderEnv = env;
    Modloader::setInfo(info);
    Modloader::modloaderPath = dirPath;
    Modloader::construct_mods();

    return iface;
}

extern "C" void modloader_accept_unity_handle(void* uhandle) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_accept_unity_handle called with uhandle: 0x%p", uhandle);

    init_all_mods();
}

#pragma region C API
extern "C" const char* get_info_id(ModInfo* instance) {
    return instance->id.c_str();
}
extern "C" void set_info_id(ModInfo* instance, const char* name) {
    instance->id = name;
}
extern "C" const char* get_info_version(ModInfo* instance) {
    return instance->version.c_str();
}
extern "C" void set_info_version(ModInfo* instance, const char* version) {
    instance->version = version;
}
extern "C" const char* get_modloader_name(ModloaderInfo* instance) {
    return instance->name.c_str();
}
extern "C" const char* get_modloader_tag(ModloaderInfo* instance) {
    return instance->tag.c_str();
}

CHECK_MODLOADER_PRELOAD;
CHECK_MODLOADER_MAIN;
CHECK_MODLOADER_ACCEPT_UNITY_HANDLE;