#include <jni.h>
#include <sys/types.h>
#include <riru.h>
#include <malloc.h>
#include <cerrno>
#include <string>
#include <xhook.h>

#include "logging.h"
#include "nativehelper/scoped_utf_chars.h"

#define schars(S,X) ScopedUtfChars S(env, *X)

static const char *blockPath;
static bool install = false;

static int (*orig_open)(const char *pathname, int flags, mode_t mode);
static int new_open(const char *pathname, int flags, mode_t mode) {
    if (strncmp(pathname, blockPath, strlen(blockPath)) == 0) {
        LOGD("redirect open %s to /dev/null", pathname);

        return orig_open("/dev/null", flags, mode);
    }

    return orig_open(pathname, flags, mode);
}

static int (*orig_mkdir)(const char *pathname, mode_t mode);
static int (new_mkdir)(const char *pathname, mode_t mode) {
    if (strncmp(pathname, blockPath, strlen(blockPath)) == 0) {
        LOGD("ignore creating directory %s", pathname);

        errno = 0;

        return 0;
    }

    return orig_mkdir(pathname, mode);
}

static int (*orig_access)(const char *pathname, int mode);
static int (new_access)(const char *pathname, int mode) {
    if (strncmp(pathname, blockPath, strlen(blockPath)) == 0) {
        LOGD("redirect access %s to /dev/null", pathname);

        return orig_access("/dev/null", mode);
    }

    return orig_access(pathname, mode);
}

static void canonicalPath(char *buf) {
    char *wp = buf;
    int skip = 0;
    for (const char *p = buf; *p != 0; p++) {
        if (*p == '/') {
            if (skip) {
                continue;
            }

            skip = 1;
        } else {
            skip = 0;
        }

        *wp++ = *p;
    }

    *wp = 0;
}

bool RegisterHook(const char* name, void* replace, void** backup) {
    int ret = xhook_register(".*", name, replace, backup);
    if (ret != 0) {
        LOGE("Failed to hook %s", name);
        return true;
    }
    return false;
}

void RegisterHooks() {
    xhook_enable_debug(1);
    xhook_enable_sigsegv_protection(0);
    bool failed = false;
#define HOOK(NAME) \
failed = failed || RegisterHook(#NAME, reinterpret_cast<void*>(new_##NAME), reinterpret_cast<void**>(&orig_##NAME))

    HOOK(open);
    HOOK(mkdir);
    HOOK(access);

#undef HOOK

    if (failed || xhook_refresh(0)) {
        LOGE("Failed to register hooks!");
        return;
    }
    xhook_clear();
}

static void specializeAppProcessPre(
        JNIEnv *env, jclass clazz, jint *uid, jint *gid, jintArray *gids, jint *runtimeFlags,
        jobjectArray *rlimits, jint *mountExternal, jstring *seInfo, jstring *jniceName,
        jboolean *startChildZygote, jstring *instructionSet, jstring *appDataDir,
        jboolean *isTopApp, jobjectArray *pkgDataInfoList, jobjectArray *whitelistedDataInfoList,
        jboolean *bindMountAppDataDirs, jboolean *bindMountAppStorageDirs) {
    // Called "before" com_android_internal_os_Zygote_nativeSpecializeAppProcess in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    // Parameters are pointers, you can change the value of them if you want
    // Some parameters are not exist is older Android versions, in this case, they are null or 0
    schars(niceName, jniceName);

    install = strcmp(niceName.c_str(), "android.process.media") == 0;

    if (!install) {
        riru_set_unload_allowed(true);
    }
}

static void specializeAppProcessPost(
        JNIEnv *env, jclass clazz) {
    // Called "after" com_android_internal_os_Zygote_nativeSpecializeAppProcess in frameworks/base/core/jni/com_android_internal_os_Zygote.cpp
    if (install) {
        jclass cEnvironment = env->FindClass("android/os/Environment");
        jmethodID mGetDirectory = env->GetStaticMethodID(cEnvironment, "getExternalStorageDirectory", "()Ljava/io/File;");
        jclass cFile = env->FindClass("java/io/File");
        jmethodID mAbsolutePath = env->GetMethodID(cFile, "getAbsolutePath", "()Ljava/lang/String;");

        jobject oDirectory = env->CallStaticObjectMethod(cEnvironment, mGetDirectory);
        auto *sPath = reinterpret_cast<jstring *>(reinterpret_cast<jstring>(env->CallObjectMethod(
                oDirectory, mAbsolutePath)));

        schars(sPath_, sPath);
        const char *path = sPath_.c_str();
        char buffer[PATH_MAX] = {0};
        snprintf(buffer, sizeof(buffer) - 1, "%s/%s", path, ".xlDownload");
        canonicalPath(buffer);
        blockPath = strdup(buffer);
        RegisterHooks();
        LOGD("installed blockPath = %s", blockPath);
    }
    // When unload allowed is true, the module will be unloaded (dlclose) by Riru
    // If this modules has hooks installed, DONOT set it to true, or there will be SIGSEGV
    // This value will be automatically reset to false before the "pre" function is called
    riru_set_unload_allowed(true);
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
                .versionName = "RIRU_MODULE_VERSION_NAME",
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
            riru_api_version = core_max_api_version <= RIRU_MODULE_API_VERSION ? core_max_api_version : RIRU_MODULE_API_VERSION;
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
