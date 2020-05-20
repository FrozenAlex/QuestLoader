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

#undef TAG
#define TAG "libmodloader"

#define MOD_PATH_FMT "/sdcard/Android/data/%s/files/mods/"
#define LIBS_PATH_FMT "/sdcard/Android/data/%s/files/libs/"
#define MOD_TEMP_PATH_FMT "/data/data/%s/cache/"

// There should only be ONE modloader PER GAME
// Ideally, there is only ONE modloader per libmodloader.so
class Modloader {
    public:
        static const std::string getLibIl2CppPath();
        static const std::string getApplicationId();
        static const bool getAllConstructed();
        static const ModloaderInfo getInfo();
        static const std::unordered_map<std::string, const Mod> getMods();
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
        static const bool setDataDirs();
        static ModloaderInfo info;
        static std::unordered_map<std::string, Mod> mods;
        static void* construct_mod(const char* full_path);
        static void setup_mod(void *handle, ModInfo& info);
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

// Generic utility functions
#pragma region Generic Utilities
static JavaVM* vm = nullptr;

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

int mkpath(char* file_path, mode_t mode) {
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
// MUST BE CALLED BEFORE LOADING MODS
const bool Modloader::setDataDirs()
{
    FILE *cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
        //not sure what the actual max is, but path_max should cover it
        char application_id[PATH_MAX] = {0};
        fread(application_id, sizeof(application_id), 1, cmdline);
        fclose(cmdline);
        trimWhitespace(application_id);
        applicationId = std::string(application_id);
        modPath = string_format(MOD_PATH_FMT, application_id);
        libsPath = string_format(LIBS_PATH_FMT, application_id);
        modTempPath = string_format(MOD_TEMP_PATH_FMT, application_id);
        // std::sprintf(modPath, MOD_PATH_FMT, application_id);
        // std::sprintf(libsPath, LIBS_PATH_FMT, application_id);
        // std::sprintf(modTempPath, MOD_TEMP_PATH_FMT, application_id);
        return 0;
    } else {
        return -1;
    }    
}

// TODO Find a way to avoid calling constructor on mods that have offsetless hooks in constructor
// Loads the mod at the given full_path
// Returns the dlopened handle
void* Modloader::construct_mod(const char* full_path) {
    // Calls the constructor on the mod by loading it
    logpfm(ANDROID_LOG_INFO, "Constructing mod: %s", full_path);
    int infile = open(full_path, O_RDONLY);
    off_t filesize = lseek(infile, 0, SEEK_END);
    lseek(infile, 0, SEEK_SET);

    const char* filename = basename(full_path);
    std::string temp_path(modTempPath);
    temp_path.append(filename);

    int outfile = open(temp_path.c_str(), O_CREAT | O_WRONLY);
    sendfile(outfile, infile, 0, filesize);
    close(infile);
    close(outfile);
    chmod(temp_path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
    auto *ret = dlopen(temp_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    unlink(temp_path.c_str());
    return ret;
}

// Calls the setup(const Modloader*, ModInfo&) function on the mod, if it exists
// This will be immediately after mod construction
void Modloader::setup_mod(void *handle, ModInfo& modInfo) {
    logpfm(ANDROID_LOG_VERBOSE, "Setting up mod handle: %p", handle);
    void (*setup_func)(const Modloader*, ModInfo&);
    *(void**)(&setup_func) = dlsym(handle, "setup");
    logpfm(ANDROID_LOG_VERBOSE, "Calling setup function: %p", setup_func);
    if (setup_func) {
        // We don't need to pass in a Modloader pointer because we have one in static anyways!
        setup_func(nullptr, modInfo);
    }
}

void Modloader::construct_mods() noexcept {
    libIl2CppPath = modloaderPath + "/libil2cpp.so";
    logpfm(ANDROID_LOG_DEBUG, "libil2cpp path: %s", libIl2CppPath.data());
    logpfm(ANDROID_LOG_DEBUG, "Constructing mods from modloader path: '%s'", modloaderPath.data());
    bool modReady = true;
    if (!setDataDirs())
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to determine data directories.");
        modReady = false;
    }
    else if (mkpath(modPath.data(), 0) != 0)
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create mod path at '%s'", modPath.c_str());
        modReady = false;
    }
    else if (mkpath(libsPath.data(), 0) != 0) 
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create library path at: '%s'", libsPath.c_str());
        modReady = false;
    }
    else if (mkpath(modTempPath.data(), 0) != 0)
    {
        logpfm(ANDROID_LOG_ERROR, "Unable to access or create mod temporary path at '%s'", modTempPath.c_str());
        modReady = false;
    }
    if (!modReady) {
        logpfm(ANDROID_LOG_ERROR, "QuestHook failed to initialize, mods will not load.");
        return;
    }

    logpfm(ANDROID_LOG_INFO, "Constructing all mods!");

    struct dirent *dp;
    DIR *dir = opendir(modPath.c_str());
    if (dir == NULL) {
        logpfm(ANDROID_LOG_ERROR, "construct_mods(%s): %s: null dir! errno: %i, msg: %s", modloaderPath.data(), modPath.c_str(), errno, strerror(errno));
        return;
    }

    // Set environment variable for shared library linking
    // Needs to include:
    // modloader
    // libs folder
    // files folder
    std::string newPath = modloaderPath + ":" + libsPath + ":" + modPath;
    char *existingLDPath = getenv("LD_LIBRARY_PATH");
    if (existingLDPath != NULL) {
        logpfm(ANDROID_LOG_DEBUG, "New LD_LIBRARY_PATH: %s", newPath.c_str());
        newPath = std::string(existingLDPath) + ":" + newPath;
    } else {
        logpfm(ANDROID_LOG_DEBUG, "Existing LD_LIBRARY_PATH does not exist!");
    }
    logpfm(ANDROID_LOG_DEBUG, "New LD_LIBRARY_PATH: %s", newPath.c_str());
    setenv("LD_LIBRARY_PATH", newPath.c_str(), 1);

    while ((dp = readdir(dir)) != NULL)
    {
        if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so"))
        {
            std::string full_path(modPath);
            full_path.append(dp->d_name);
            auto *modHandle = construct_mod(full_path.c_str());
            ModInfo modInfo;
            setup_mod(modHandle, modInfo);
            logpfm(ANDROID_LOG_VERBOSE, "Creating mod with name: %s, id: %s, version: %s, path: %s, handle: %p", dp->d_name, modInfo.id.c_str(), modInfo.version.c_str(), full_path.c_str(), modHandle);
            // Don't overwrite existing mod IDs, warn when doing so
            if (!mods.try_emplace(modInfo.id, dp->d_name, full_path, modInfo, modHandle).second) {
                logpfm(ANDROID_LOG_WARN, "Could not construct mod with name: %s, id: %s, version: %s because another mod of that same id exists!", dp->d_name, modInfo.id.c_str(), modInfo.version.c_str());
            }
        }
    }
    closedir(dir);
    if (existingLDPath == NULL) {
        logpfm(ANDROID_LOG_VERBOSE, "Unsetting LD_LIBRARY_PATH!");
        unsetenv("LD_LIBRARY_PATH");
    } else {
        logpfm(ANDROID_LOG_VERBOSE, "Resetting LD_LIBRARY_PATH to: %s", existingLDPath);
        setenv("LD_LIBRARY_PATH", existingLDPath, 1);
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
    dlclose(imagehandle);
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
        mod.second.load_mod();
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
const bool Modloader::getAllConstructed() {
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
#pragma endregion

// Mod functionality
#pragma region Mod Functions
const bool Mod::get_loaded() const {
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
    logpf(ANDROID_LOG_VERBOSE, "Calling init function: %p", init_func);
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
    Modloader::setInfo(info);
    Modloader::modloaderPath = dirPath;
    Modloader::construct_mods();

    return iface;
}

extern "C" void modloader_accept_unity_handle(void* uhandle) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_accept_unity_handle called with uhandle: 0x%p", uhandle);

    init_all_mods();
}

CHECK_MODLOADER_PRELOAD;
CHECK_MODLOADER_MAIN;
CHECK_MODLOADER_ACCEPT_UNITY_HANDLE;