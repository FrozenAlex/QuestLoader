#include <libmain.hpp>
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

#include "log.hpp"

#include <sys/mman.h>

#include "../../../beatsaber-hook/shared/inline-hook/inlineHook.h"
#include "../../../beatsaber-hook/shared/utils/utils.h"
#include "../../../beatsaber-hook/shared/inline-hook/And64InlineHook.hpp"
#include "modloader.hpp"

#undef TAG
#define TAG "libmodloader"

#define MOD_PATH_FMT "/sdcard/Android/data/%s/files/mods/"
#define MOD_TEMP_PATH_FMT "/data/data/%s/cache/curmod.so"

char *modPath;
char *modTempPath;

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

// MUST BE CALLED BEFORE LOADING MODS
const int setDataDirs()
{
    FILE *cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
        //not sure what the actual max is, but path_max should cover it
        char application_id[PATH_MAX] = {0};
        fread(application_id, sizeof(application_id), 1, cmdline);
        fclose(cmdline);
        trimWhitespace(application_id);
        modTempPath = (char*)malloc(PATH_MAX);
        modPath = (char*)malloc(PATH_MAX);
        std::sprintf(modPath, MOD_PATH_FMT, application_id);
        std::sprintf(modTempPath, MOD_TEMP_PATH_FMT, application_id);
        return 0;
    } else {
        return -1;
    }    
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

// TODO Find a way to avoid calling constructor on mods that have offsetless hooks in constructor
// Loads the mod at the given full_path
// Returns the dlopened handle
void* construct_mod(const char* full_path) {
    // Calls the constructor on the mod by loading it
    logpf(INFO, "Constructing mod: %s", full_path);
    int infile = open(full_path, O_RDONLY);
    off_t filesize = lseek(infile, 0, SEEK_END);
    lseek(infile, 0, SEEK_SET);
    unlink(modTempPath);
    int outfile = open(modTempPath, O_CREAT | O_WRONLY);
    sendfile(outfile, infile, 0, filesize);
    close(infile);
    close(outfile);
    chmod(modTempPath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
    return dlopen(modTempPath, RTLD_NOW);
}

// Calls the init() function on the mod, if it exists
// This will be before il2cpp functionality is available
// Called in preload
void init_mod(void* handle) {
    void (*init)(void);
    *(void**)(&init) = dlsym(handle, "init");
    if (init) {
        init();
    }
}

// Calls the preload() function on the mod, if it exists
// This will be before il2cpp functionality is available
// Called in accept_unity_handle
void preload_mod(void* handle) {
    void (*preload)(void);
    *(void**)(&preload) = dlsym(handle, "preload");
    if (preload) {
        preload();
    }
}

// Calls the load() function on the mod, if it exists
// This will be after il2cpp functionality is available
// Called immediately after il2cpp_init
void load_mod(void* handle) {
    void (*load)(void);
    *(void**)(&load) = dlsym(handle, "load");
    if (load) {
        load();
    }
}

// Holds all constructed mods' full paths and dlopen handles
static std::vector<std::pair<const std::string, void*>> mods;
// Whether the mods have been constructed via construct_mods
static bool constructed = false;

void construct_mods() noexcept {
    logpf(INFO, "Constructing all mods!");

    struct dirent *dp;
    DIR *dir = opendir(modPath);
    if (dir == NULL) {
        logpf(ERROR, "construct_mods(%s): null dir! errno: %i, msg: %s", modPath, errno, strerror(errno));
        return;
    }

    while ((dp = readdir(dir)) != NULL)
    {
        if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so"))
        {
            std::string full_path(modPath);
            full_path.append(dp->d_name);
            auto modHandle = construct_mod(full_path.c_str());
            mods.push_back({full_path, modHandle});
        }
    }
    closedir(dir);
    constructed = true;
    logpf(INFO, "Done constructing mods!");
}

// Calls the init functions on all constructed mods
void init_mods() noexcept {
    if (!constructed) {
        logpf(ERROR, "Tried to initalize mods, but they are not yet constructed!");
        return;
    }
    logpf(INFO, "Initializing all mods!");

    for (auto mod : mods) {
        logpf(INFO, "Initializing mod: %s", mod.first.c_str());
        init_mod(mod.second);
    }

    logpf(INFO, "Initialized all mods!");
}

// Calls the preload functions on all constructed mods
void preload_mods() noexcept {
    if (!constructed) {
        logpf(ERROR, "Tried to preload mods, but they are not yet constructed!");
        return;
    }
    logpf(INFO, "Preloading all mods!");

    for (auto mod : mods) {
        logpf(INFO, "Preloading mod: %s", mod.first.c_str());
        preload_mod(mod.second);
    }

    logpf(INFO, "Preloading all mods!");
}

// Calls the load functions on all constructed mods
void load_mods() noexcept {
    if (!constructed) {
        logpf(ERROR, "Tried to load mods, but they are not yet constructed!");
        return;
    }
    logpf(INFO, "Loading all mods!");

    for (auto mod : mods) {
        logpf(INFO, "Loading mod: %s", mod.first.c_str());
        load_mod(mod.second);
    }

    logpf(INFO, "Loaded all mods!");
}

static void* imagehandle;
static void (*il2cppInit)(const char* domain_name);
// Loads the mods after il2cpp has been initialized
MAKE_HOOK_OFFSETLESS(il2cppInitHook, void, const char* domain_name)
{
    il2cppInitHook(domain_name);
    dlclose(imagehandle);
    load_mods();
}

extern "C" void modloader_preload() noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_preload called (should be really early)");

    logpf(INFO, "Welcome!");

    int modReady = 0;
    if (setDataDirs() != 0)
    {
         logpf(ERROR, "Unable to determine data directories.");
        modReady = -1;
    }
    else if (mkpath(modPath, 0) != 0)
    {
        logpf(ERROR, "Unable to access or create mod path at '%s'", modPath);
        modReady = -1;
    }
    else if (mkpath(modTempPath, 0) != 0)
    {
        logpf(ERROR, "Unable to access or create mod temporary path at '%s'", modTempPath);
        modReady = -1;
    }
    if (modReady != 0) {
        logpf(ERROR, "QuestHook failed to initialize, mods will not load.");
        return;
    }

    construct_mods();
}

extern "C" JNINativeInterface modloader_main(JavaVM* vm, JNIEnv* env, std::string_view loadSrc) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_main called with vm: 0x%p, env: 0x%p, loadSrc: %s", vm, env, loadSrc.data());

    auto iface = jni::interface::make_passthrough_interface<JNINativeInterface>(&env->functions);

    init_mods();

    return iface;
}

extern "C" void modloader_accept_unity_handle(void* uhandle) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_accept_unity_handle called with uhandle: 0x%p", uhandle);

    preload_mods();

    imagehandle = dlopen(IL2CPP_SO_PATH, RTLD_LOCAL | RTLD_LAZY);
    *(void**)(&il2cppInit) = dlsym(imagehandle, "il2cpp_init");
	logpf(INFO, "Loaded: il2cpp_init (%p)", il2cppInit);
    if (il2cppInit) {
        INSTALL_HOOK_DIRECT(il2cppInitHook, il2cppInit);
    } else {
        logpf(ERROR, "Failed to dlsym il2cpp_init!");
    }
}

CHECK_MODLOADER_PRELOAD;
CHECK_MODLOADER_MAIN;
CHECK_MODLOADER_ACCEPT_UNITY_HANDLE;