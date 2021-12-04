#include <jni.h>
#include <sys/types.h>
#include <riru.h>
#include <malloc.h>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include "logging.h"
#include <dirent.h>
#include <nativehelper/scoped_utf_chars.h>
#include "dobby.h"
#include <unordered_set>
#include <string.h>

#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x) __builtin_expect(!!(x), 1)

namespace android {

    static int apiLevel = 0;
    static bool prefer_system = false;
    static int GetApiLevel() {
        if (LIKELY(apiLevel > 0)) return apiLevel;

        char buf[PROP_VALUE_MAX + 1];
        if (LIKELY(__system_property_get("ro.build.version.sdk", buf)) > 0)
            apiLevel = strtol(buf, nullptr, 10);
        if (UNLIKELY(apiLevel == 0))
            //Assume android 11 if any problem
            apiLevel = 30;

        return apiLevel;
    }
}

namespace Config {
    
    static int foreach_dir(const char *path, void(*callback)(int, struct dirent *)) {
        DIR *dir;
        struct dirent *entry;
        int fd;
        if (UNLIKELY((dir = opendir(path)) == nullptr)) {
            LOGE("Failed to open path %s", path);
            return -1;
        }
        fd = dirfd(dir);
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            callback(fd, entry);
        }
        closedir(dir);
        return 0;
    }

    struct Property {
        std::string name;
        std::string value;
        int ser;

        Property(const char *name, const char *value, int ser) : name(name), value(value),
                                                                      ser(ser) {}
    };

    namespace Properties {
        static Property *Find(const char *name);

        static void Put(const char *name, const char *value);

    }
    namespace Packages {
        static bool Find(const char *name);

        static void Add(const char *name);
    }

#define CONFIG_PATH "/data/misc/mph/config"
#define PROPS_PATH CONFIG_PATH "/properties"
#define PACKAGES_PATH CONFIG_PATH "/packages"

    static std::map<std::string, Property *> props;
    static std::unordered_set<std::string> packages;

    Property *Properties::Find(const char *name) {
        if (UNLIKELY(!name)) return nullptr;
        auto it = props.find(name);
        if (LIKELY(it != props.end())) {
            return it->second;
        }
        return nullptr;
    }

    void Properties::Put(const char *name, const char *value) {
        if (UNLIKELY(!name)) return;
        auto prop = Find(name);
        delete prop;
        props[name] = new Property(name, value ? value : "", strlen(value)<<24);
    }

    bool Packages::Find(const char *name) {
        if (UNLIKELY(!name)) return false;
        return packages.find(name) != packages.end();
    }

    void Packages::Add(const char *name) {
        if (UNLIKELY(!name)) return;
        packages.insert(name);
    }

    inline bool rmnewline(char *str) {
        bool hasnl = false;
        int size = strlen(str);
        for (int tmp = 0; tmp < size; tmp++) {
            if (str[tmp] == '\n') {
                str[tmp] = 0;
                hasnl = true;
            }
        }
        return hasnl;
    }

    static void Load() {
        foreach_dir(PROPS_PATH, [](int dirfd, struct dirent *entry) {
            auto name = entry->d_name;
            if(LIKELY(!strcmp(name,"prefer_system"))) {
                android::prefer_system = true;
                return;
            }
            int fd = openat(dirfd, name, O_RDONLY);
            if (fd == -1) return;
            char buf[PROP_VALUE_MAX]{0};
            if (LIKELY(read(fd, buf, PROP_VALUE_MAX)) >= 0) {
                if(UNLIKELY(rmnewline(buf)))
                    LOGV("Detected newline in prop %s", name);
                Properties::Put(name, buf);
                LOGV("add prop %s as %s", name, buf);
            }
            close(fd);
        });
        foreach_dir(PACKAGES_PATH, [](int, struct dirent *entry) {
            auto name = entry->d_name;
            Packages::Add(name);
            LOGV("add package %s", name);
        });
        if (UNLIKELY(packages.empty()))
            LOGI("hook target package list is empty");
        if (UNLIKELY(props.empty()))
            LOGI("hook prop list is empty");
        LOGI("hook target package and prop list loaded");
    }
}

namespace Hook {

    struct prop_info_compat {
        std::atomic_uint_least32_t serial;
        char value[PROP_VALUE_MAX];
        char name[128];
    } prop_info_compat;
    struct prop_info {
        std::atomic_uint_least32_t serial;
        char value[PROP_VALUE_MAX];
        char name[0];
    };

    static std::map<std::string, struct prop_info_compat *> mprop;
    static std::unordered_set<std::string> sys_aval_props;

    static void prepare() {
        LOGV("preparing fake prop memory..");
        for (const auto &prop : Config::props) {
            static auto *ps = (struct prop_info_compat *) malloc(
                    sizeof(struct prop_info_compat));
            strcpy(ps->name, prop.first.c_str());
            strcpy(ps->value, prop.second->value.c_str());
            ps->serial.store(prop.second->ser,std::memory_order_relaxed);
            mprop[prop.first.c_str()] = ps;
            LOGV("Created prop struct in mem for %s", ps->name);
        }
    }

    prop_info *(*orig__system_property_find)(const char *name);

    prop_info *my__system_property_find(const char *name) {
        int psz = strlen(name) + 1;
        char mname[psz];
        memset(mname, 0, psz);
        strcpy(mname, name);
        auto prop = Config::Properties::Find(mname);
        prop_info *sysprop = orig__system_property_find(name);
        if (UNLIKELY(prop)) {
            auto it = mprop.find(mname);
            if (it != mprop.end()) {
                if(UNLIKELY(android::prefer_system == true && sysprop)) {
                    if(UNLIKELY(sys_aval_props.find(mname)==sys_aval_props.end())) sys_aval_props.insert(mname);
                    return sysprop;
                } else {
                    if(android::prefer_system == true && sys_aval_props.find(mname)!=sys_aval_props.end()) sys_aval_props.erase(mname);
                }
            }
                return reinterpret_cast<prop_info *>(it->second);
        } else {
            return sysprop;
        }
    }

    using callback_func = void(void *cookie, const char *name, const char *value, uint32_t serial);

    void (*orig__system_property_read_callback)(const prop_info *pi, callback_func *callback,
                                                void *cookie);

    void
    my__system_property_read_callback(const prop_info *pi, callback_func *callback, void *cookie) {
        if(UNLIKELY(Config::Properties::Find(pi->name) && !(android::prefer_system && sys_aval_props.find(pi->name)!=sys_aval_props.end())))
            return callback(cookie, pi->name, pi->value, pi->serial);
        return orig__system_property_read_callback(pi, callback, cookie);

    }

    int (*orig__system_property_get)(const char *key, char *value);

    int my__system_property_get(const char *key, char *value) {
        int res = orig__system_property_get(key, value);
        auto prop = Config::Properties::Find(key);
        if (UNLIKELY(prop)) {
            strcpy(value, prop->value.c_str());
        }
        return res;
    }

    static void InstallHook() {
        int res;
        if (LIKELY(android::apiLevel >= 30)) {
            LOGV("Installing hook for API Level %d", android::apiLevel);
            res = DobbyHook((void *) __system_property_read_callback,
                            (void *) my__system_property_read_callback,
                            (void **) &orig__system_property_read_callback);
            LOGV("Hook return %d", res);
        }
        if (LIKELY(android::apiLevel >= 26)) {
            LOGV("Installing hook for API Level %d", android::apiLevel);
            res = DobbyHook((void *) __system_property_find,
                            (void *) my__system_property_find,
                            (void **) &orig__system_property_find);
        } else {
            LOGV("Installing hook for API Level %d", android::apiLevel);
            res = DobbyHook((void *) __system_property_get,
                            (void *) my__system_property_get,
                            (void **) &orig__system_property_get);
        }
        LOGV("Hook return %d", res);
    }
}

static int shouldSkipUid(int uid) {
    // By default (if the module does not provide this function in init), Riru will only call
    // module functions in "normal app processes" (10000 <= uid % 100000 <= 19999)

    // Provide this function so that the module can control if a specific uid should be skipped

    // Riru 25:
    // This function is removed for modules which has adapted 25, means forkAndSpecialize and
    // specializeAppProcess will be called for all uids.
    return false;
}

static char saved_package_name[256] = {0};
static int saved_uid;

#ifdef DEBUG
static char saved_process_name[256] = {0};
#endif

static void appProcessPre(JNIEnv *env, jint *uid, jstring *jNiceName, jstring *jAppDataDir) {

    saved_uid = *uid;

#ifdef DEBUG
    memset(saved_process_name, 0, 256);

    if (*jNiceName) {
        sprintf(saved_process_name, "%s", ScopedUtfChars(env, *jNiceName).c_str());
    }
#endif


    if (*jAppDataDir) {
        auto appDataDir = ScopedUtfChars(env, *jAppDataDir).c_str();
        if (LIKELY((strstr(appDataDir, "/data") || strstr(appDataDir, "/mnt/expand")))) {
            const char *pkg = strrchr(appDataDir, '/');
            if (LIKELY(pkg != nullptr)) {
                strcpy(saved_package_name, ++pkg);
            } else {
                saved_package_name[0] = '\0';
            }
        } else {
            saved_package_name[0] = '\0';
        }

    }
}

static void appProcessPost(
        JNIEnv *env, const char *from, const char *package_name, jint uid) {

    LOGD("%s: uid=%d, package=%s, process=%s", from, uid, package_name, saved_process_name);

    if (UNLIKELY(Config::Packages::Find(package_name))) {
        LOGI("install hook for %d:%s", uid / 100000, package_name);
        Hook::prepare();
        Hook::InstallHook();
    } else {
        riru_set_unload_allowed(true);
    }
}

static void forkAndSpecializePre(
        JNIEnv *env, jclass clazz, jint *uid, jint *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jint *mountExternal, jstring *seInfo, jstring *niceName,
        jintArray *fdsToClose, jintArray *fdsToIgnore, jboolean *is_child_zygote,
        jstring *instructionSet, jstring *appDataDir, jboolean *isTopApp,
        jobjectArray *pkgDataInfoList,
        jobjectArray *whitelistedDataInfoList, jboolean *bindMountAppDataDirs,
        jboolean *bindMountAppStorageDirs) {
    // Called "before" com_android_internal_os_Zygote_nativeForkAndSpecialize in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    // Parameters are pointers, you can change the value of them if you want
    // Some parameters are not exist is older Android versions, in this case, they are null or 0
    appProcessPre(env, uid, niceName, appDataDir);
}

static void forkAndSpecializePost(JNIEnv *env, jclass clazz, jint res) {
    // Called "after" com_android_internal_os_Zygote_nativeForkAndSpecialize in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    // "res" is the return value of com_android_internal_os_Zygote_nativeForkAndSpecialize
    riru_set_unload_allowed(false);
    if (res == 0) {
        // In app process

        // When unload allowed is true, the module will be unloaded (dlclose) by Riru
        // If this modules has hooks installed, DONOT set it to true, or there will be SIGSEGV
        // This value will be automatically reset to false before the "pre" function is called
        appProcessPost(env, "forkAndSpecialize", saved_package_name, saved_uid);
    } else {
        riru_set_unload_allowed(true);
        // In zygote process
    }
}

static void specializeAppProcessPre(
        JNIEnv *env, jclass clazz, jint *uid, jint *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jint *mountExternal, jstring *seInfo, jstring *niceName,
        jboolean *startChildZygote, jstring *instructionSet, jstring *appDataDir,
        jboolean *isTopApp, jobjectArray *pkgDataInfoList,
        jobjectArray *whitelistedDataInfoList,
        jboolean *bindMountAppDataDirs, jboolean *bindMountAppStorageDirs) {
    // Called "before" com_android_internal_os_Zygote_nativeSpecializeAppProcess in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    // Parameters are pointers, you can change the value of them if you want
    // Some parameters are not exist is older Android versions, in this case, they are null or 0
    appProcessPre(env, uid, niceName, appDataDir);
}

static void specializeAppProcessPost(
        JNIEnv *env, jclass clazz) {
    // Called "after" com_android_internal_os_Zygote_nativeSpecializeAppProcess in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp

    // When unload allowed is true, the module will be unloaded (dlclose) by Riru
    // If this modules has hooks installed, DONOT set it to true, or there will be SIGSEGV
    // This value will be automatically reset to false before the "pre" function is called
    riru_set_unload_allowed(false);
    appProcessPost(env, "specializeAppProcess", saved_package_name, saved_uid);
}

static void forkSystemServerPre(
        JNIEnv *env, jclass clazz, uid_t *uid, gid_t *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jlong *permittedCapabilities, jlong *effectiveCapabilities) {
    // Called "before" com_android_internal_os_Zygote_forkSystemServer in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    // Parameters are pointers, you can change the value of them if you want
    // Some parameters are not exist is older Android versions, in this case, they are null or 0
}

static void forkSystemServerPost(JNIEnv *env, jclass clazz, jint res) {
    // Called "after" com_android_internal_os_Zygote_forkSystemServer in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp

    if (res == 0) {
        // In system server process
    } else {
        // In zygote process
    }
}

static void onModuleLoaded() {
    // Called when this library is loaded and "hidden" by Riru (see Riru's hide.cpp)

    // If you want to use threads, start them here rather than the constructors
    // __attribute__((constructor)) or constructors of static variables,
    // or the "hide" will cause SIGSEGV
    Config::Load();
    int al = android::GetApiLevel();
    LOGV("API Level detected as %d", al);
}


extern "C" {

int riru_api_version;
const char *riru_magisk_module_path = nullptr;
int *riru_allow_unload = nullptr;

static auto module = RiruVersionedModuleInfo{
        .moduleApiVersion = RIRU_MODULE_API_VERSION,
        .moduleInfo= RiruModuleInfo{
                .supportHide = true,
                .version = RIRU_MODULE_VERSION,
                .versionName = RIRU_MODULE_VERSION_NAME,
                .onModuleLoaded = onModuleLoaded,
                .forkAndSpecializePre = forkAndSpecializePre,
                .forkAndSpecializePost = forkAndSpecializePost,
                .forkSystemServerPre = forkSystemServerPre,
                .forkSystemServerPost = forkSystemServerPost,
                .specializeAppProcessPre = specializeAppProcessPre,
                .specializeAppProcessPost = specializeAppProcessPost
        }
};

#ifndef RIRU_MODULE_LEGACY_INIT
RiruVersionedModuleInfo *init(Riru *riru) {
    auto core_max_api_version = riru->riruApiVersion;
    riru_api_version = core_max_api_version <= RIRU_MODULE_API_VERSION ? core_max_api_version : RIRU_MODULE_API_VERSION;
    module.moduleApiVersion = riru_api_version;

    riru_magisk_module_path = strdup(riru->magiskModulePath);
    if (riru_api_version >= 25) {
        riru_allow_unload = riru->allowUnload;
    }
    return &module;
}
#else
RiruVersionedModuleInfo *init(Riru *riru) {
    static int step = 0;
    step += 1;

    switch (step) {
        case 1: {
            auto core_max_api_version = riru->riruApiVersion;
            riru_api_version =
                    core_max_api_version <= RIRU_MODULE_API_VERSION ? core_max_api_version
                                                                    : RIRU_MODULE_API_VERSION;
            if (riru_api_version < 25) {
                module.moduleInfo.unused = (void *) shouldSkipUid;
            } else {
                riru_allow_unload = riru->allowUnload;
            }
            if (riru_api_version >= 24) {
                module.moduleApiVersion = riru_api_version;
                riru_magisk_module_path = strdup(riru->magiskModulePath);
                return &module;
            } else {
                return (RiruVersionedModuleInfo *) &riru_api_version;
            }
        }
        case 2: {
            return (RiruVersionedModuleInfo *) &module.moduleInfo;
        }
        case 3:
        default: {
            return nullptr;
        }
    }
}
#endif
}
