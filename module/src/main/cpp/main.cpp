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

#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x) __builtin_expect(!!(x), 1)

namespace Config {
    int foreach_dir(const char *path, void(*callback)(int, struct dirent *)) {
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
        unsigned ser;

        Property(const char *name, const char *value,unsigned ser) : name(name), value(value), ser(ser) {}
    };

    namespace Properties {
        Property *Find(const char *name);

        void Put(const char *name, const char *value);
    }
    namespace Packages {
        bool Find(const char *name);

        void Add(const char *name);
    }

#define CONFIG_PATH "/data/misc/mph/config"
#define PROPS_PATH CONFIG_PATH "/properties"
#define PACKAGES_PATH CONFIG_PATH "/packages"

    static std::map<std::string, Property *> props;
    static std::vector<std::string> packages;
    static int serm = 10086;

    Property *Properties::Find(const char *name) {
        if (UNLIKELY(!name)) return nullptr;
        auto it = props.find(name);
        if (LIKELY(it != props.end())) {
            return it->second;
        }
        return nullptr;
    }

    void Properties::Put(const char *name, const char *value) {
        if (!name) return;
        auto prop = Find(name);
        delete prop;
        props[name] = new Property(name, value ? value : "",serm++);
    }

    bool Packages::Find(const char *name) {
        if (!name) return false;
        return std::find(packages.begin(), packages.end(), name) != packages.end();
    }

    void Packages::Add(const char *name) {
        if (!name) return;
        packages.emplace_back(name);
    }

    void Load() {
        foreach_dir(PROPS_PATH, [](int dirfd, struct dirent *entry) {
            auto name = entry->d_name;
            int fd = openat(dirfd, name, O_RDONLY);
            if (fd == -1) return;
            char buf[PROP_VALUE_MAX]{0};
            if (LIKELY(read(fd, buf, PROP_VALUE_MAX)) >= 0) {
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
        if (packages.empty())
            LOGI("hook target package list is empty");
        if (props.empty())
            LOGI("hook prop list is empty");
        LOGI("hook target package and prop list loadded");
    }
}
namespace Hook {

    struct prop_info_compat {
        char name[128];
        unsigned volatile serial;
        char value[PROP_VALUE_MAX];
    } prop_info_compat;


    prop_info *(*orig__system_property_find)(const char *name);

    prop_info *my__system_property_find(const char *name) {
        char mname[128] = {0};
        strcpy(mname,name);
        auto prop = Config::Properties::Find(mname);
        if(UNLIKELY(prop)){
            auto *mpi = (struct prop_info_compat *)malloc(sizeof(prop_info_compat));
            strcpy(mpi->name,mname);
            strcpy(mpi->value,prop->value.c_str());
            mpi->serial = prop->ser;
            return reinterpret_cast<prop_info *>(mpi);
        }
        else{
            return orig__system_property_find(name);
        }
    }
/*
    using callback_func = void(void *cookie, const char *name, const char *value, uint32_t serial);
    thread_local callback_func *saved_callback = nullptr;

    static void my_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
        if (!saved_callback) return;
        LOGV("accessing prop %s as %s",name,value);
        LOGV("savename is %s",saved_name);
        if (UNLIKELY(strcmp(name,stubname))) {
            if (LIKELY(!strcmp(saved_name, "nope"))) {
                auto prop = Config::Properties::Find(saved_name);
                if (LIKELY(prop)) {
                    LOGV("replace prop %s from %s to %s",name,value,prop->value.c_str());
                    return saved_callback(cookie, name, prop->value.c_str(), serial);
                } else {
                    return saved_callback(cookie, name, value, serial);
                }
            } else {
                return saved_callback(cookie, name, value, serial);
            }
        } else {
            return saved_callback(cookie, name, value, serial);
        }
    }

    void (*orig__system_property_read_callback)(const prop_info *pi, callback_func *callback,
                                                void *cookie);

    void
    my__system_property_read_callback(const prop_info *pi, callback_func *callback, void *cookie) {
        saved_callback = callback;
        orig__system_property_read_callback(pi, my_callback, cookie);
    }
*/
    static void InstallHook() {
        LOGV("Installing Hook");
        int res;
        res = DobbyHook(DobbySymbolResolver("libc.so", "__system_property_find"),
                        (void *) my__system_property_find, (void **) &orig__system_property_find);
        LOGV("Hook return %d", res);
        /*
        res = DobbyHook(DobbySymbolResolver("libc.so", "__system_property_read_callback"),
                        (void *) my__system_property_read_callback,
                        (void **) &orig__system_property_read_callback);
        LOGV("Hook return %d", res);
         */
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

    memset(saved_package_name, 0, 256);

    if (*jAppDataDir) {
        auto appDataDir = ScopedUtfChars(env, *jAppDataDir).c_str();
        int user = 0;

        // /data/user/<user_id>/<package>
        if (sscanf(appDataDir, "/data/%*[^/]/%d/%s", &user, saved_package_name) == 2)
            goto found;

        // /mnt/expand/<id>/user/<user_id>/<package>
        if (sscanf(appDataDir, "/mnt/expand/%*[^/]/%*[^/]/%d/%s", &user, saved_package_name) ==
            2)
            goto found;

        // /data/data/<package>
        if (sscanf(appDataDir, "/data/%*[^/]/%s", saved_package_name) == 1)
            goto found;

        // nothing found
        saved_package_name[0] = '\0';

        found:;
    }
}

void injectBuild(JNIEnv *env) {
    if (UNLIKELY(env == nullptr)) {
        LOGW("failed to inject android.os.Build for %s due to env is null", saved_package_name);
        return;
    }
    LOGI("inject android.os.Build for %s ", saved_package_name);

    jclass build_class = env->FindClass("android/os/Build");
    if (UNLIKELY(build_class == nullptr)) {
        LOGW("failed to inject android.os.Build for %s due to build is null",
             saved_package_name);
        return;
    }

    jstring new_str = env->NewStringUTF("Xiaomi");

    jfieldID brand_id = env->GetStaticFieldID(build_class, "BRAND", "Ljava/lang/String;");
    if (UNLIKELY(brand_id != nullptr)) {
        env->SetStaticObjectField(build_class, brand_id, new_str);
    }

    jfieldID manufacturer_id = env->GetStaticFieldID(build_class, "MANUFACTURER",
                                                     "Ljava/lang/String;");
    if (LIKELY(manufacturer_id != nullptr)) {
        env->SetStaticObjectField(build_class, manufacturer_id, new_str);
    }

    jfieldID product_id = env->GetStaticFieldID(build_class, "PRODUCT", "Ljava/lang/String;");
    if (UNLIKELY(product_id != nullptr)) {
        env->SetStaticObjectField(build_class, product_id, new_str);
    }

    if (UNLIKELY(env->ExceptionCheck())) {
        env->ExceptionClear();
    }

    env->DeleteLocalRef(new_str);
}

static void appProcessPost(
        JNIEnv *env, const char *from, const char *package_name, jint uid) {

    LOGD("%s: uid=%d, package=%s, process=%s", from, uid, package_name, saved_process_name);

    if (UNLIKELY(Config::Packages::Find(package_name))) {
        LOGI("install hook for %d:%s", uid / 100000, package_name);
        injectBuild(env);
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