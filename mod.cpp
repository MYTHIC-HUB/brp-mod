// BRP Game Mod - ARMv7a (32-bit)
// Engine: GTA SA Android (custom BRP fork)
// Features: Aimbot (target detection) | Friend List | ESP

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <utility>

#define LOG_TAG "BRPMOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static uintptr_t g_base = 0;

static uintptr_t get_base() {
    if (g_base) return g_base;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libGAME.so") && strstr(line, "r-xp")) {
            g_base = (uintptr_t)strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return g_base;
}

// =====================================================================
// OFFSETS 32-BIT (Extraidos via readelf do libGAME.so)
// =====================================================================
#define OFFSET_PED_POOL         0x0095ac44
#define OFFSET_GetPed           0x0040d491
#define OFFSET_FindPlayerPed    0x0040b2d9

// Estrutura do CPed (32-bit)
#define PED_SIZE                0x7C4
#define PED_OFFSET_MATRIX       0x34
#define PED_OFFSET_HEALTH       0x540

#define MATRIX_POS_X            0x30
#define MATRIX_POS_Y            0x34
#define MATRIX_POS_Z            0x38

struct CPool_t {
    uintptr_t m_pObjects;
    uintptr_t pad;
    int       m_nSize;
    int       m_nUsed;
    uintptr_t m_byteMap;
};

static std::vector<int> g_friends = {};
static bool  g_aimbot_enabled = false;
static bool  g_esp_enabled    = false;
static float g_aimbot_range   = 150.0f;

struct Vec3 { float x, y, z; };

static float dist3d(Vec3 a, Vec3 b) {
    float dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static Vec3 get_ped_pos(uintptr_t ped) {
    Vec3 v = {0,0,0};
    if (!ped) return v;
    uintptr_t matrix = ped + PED_OFFSET_MATRIX;
    v.x = *(float*)(matrix + MATRIX_POS_X);
    v.y = *(float*)(matrix + MATRIX_POS_Y);
    v.z = *(float*)(matrix + MATRIX_POS_Z);
    return v;
}

static float get_ped_health(uintptr_t ped) {
    if (!ped) return 0.f;
    return *(float*)(ped + PED_OFFSET_HEALTH);
}

static bool is_ped_alive(uintptr_t ped) {
    if (!ped) return false;
    float hp = get_ped_health(ped);
    return hp > 0.0f && hp < 1000.0f; 
}

typedef uintptr_t (*fn_GetPed_t)(int index);
typedef uintptr_t (*fn_FindPlayerPed_t)(int index);

static fn_GetPed_t        fn_GetPed        = nullptr;
static fn_FindPlayerPed_t fn_FindPlayerPed = nullptr;

static void init_funcs() {
    uintptr_t base = get_base();
    if (!base) return;
    fn_GetPed        = (fn_GetPed_t)       (base + OFFSET_GetPed);
    fn_FindPlayerPed = (fn_FindPlayerPed_t)(base + OFFSET_FindPlayerPed);
}

static std::vector<std::pair<int,uintptr_t>> get_all_peds_mode(bool bit7_is_free) {
    std::vector<std::pair<int,uintptr_t>> result;
    uintptr_t base = get_base();
    if (!base) return result;

    uintptr_t pool = *(uintptr_t*)(base + OFFSET_PED_POOL);
    if (!pool) return result;

    CPool_t* p = (CPool_t*)pool;
    int cap = p->m_nSize;
    if (cap <= 0 || cap > 140) return result;

    for (int i = 0; i < cap; i++) {
        uint8_t flag = *(uint8_t*)(p->m_byteMap + i);
        bool used = bit7_is_free ? ((flag & 0x80) == 0) : ((flag & 0x80) != 0);
        if (!used) continue;

        uintptr_t ped = p->m_pObjects + (uintptr_t)(i * PED_SIZE);
        if (!is_ped_alive(ped)) continue;

        Vec3 pos = get_ped_pos(ped);
        if (fabsf(pos.x) > 5000.f || fabsf(pos.y) > 2000.f || fabsf(pos.z) > 5000.f) continue;

        result.push_back({i, ped});
    }
    return result;
}

// Descobre automaticamente qual e a convencao de flag do pool e memoriza
enum PoolMode { POOL_UNKNOWN, POOL_BIT7_FREE, POOL_BIT7_USED };
static PoolMode g_pool_mode = POOL_UNKNOWN;

static std::vector<std::pair<int,uintptr_t>> get_all_peds() {
    if (g_pool_mode == POOL_UNKNOWN) {
        auto a = get_all_peds_mode(true);
        auto b = get_all_peds_mode(false);
        LOGI("pool probe: BIT7_FREE=%zu ped(s), BIT7_USED=%zu ped(s)", a.size(), b.size());
        if (a.size() >= b.size() && !a.empty()) {
            g_pool_mode = POOL_BIT7_FREE; return a;
        } else {
            g_pool_mode = POOL_BIT7_USED; return b;
        }
    }
    return get_all_peds_mode(g_pool_mode == POOL_BIT7_FREE);
}

// =====================================================================
// JNI EXPORTS
// =====================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_brp_mod_ModMenu_addFriend(JNIEnv*, jclass, jint id) {
    for (int x : g_friends) if (x == (int)id) return;
    g_friends.push_back((int)id);
}
extern "C" JNIEXPORT void JNICALL
Java_com_brp_mod_ModMenu_removeFriend(JNIEnv*, jclass, jint id) {
    for (auto it = g_friends.begin(); it != g_friends.end(); ++it) {
        if (*it == (int)id) { g_friends.erase(it); break; }
    }
}

static bool is_friend(int pedIndex) {
    for (int x : g_friends) if (x == pedIndex) return true;
    return false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_brp_mod_ModMenu_setAimbot(JNIEnv*, jclass, jboolean en) {
    g_aimbot_enabled = (bool)en;
}
extern "C" JNIEXPORT void JNICALL
Java_com_brp_mod_ModMenu_setESP(JNIEnv*, jclass, jboolean en) {
    g_esp_enabled = (bool)en;
}
extern "C" JNIEXPORT void JNICALL
Java_com_brp_mod_ModMenu_setRange(JNIEnv*, jclass, jfloat range) {
    g_aimbot_range = range;
}

// O buffer de ESP e preenchido na thread e lido pelo Java
static char g_esp_buf[8192];
static Vec3 g_aimbot_target = {0,0,0};
static bool g_has_target = false;

static void mod_thread_tick() {
    uintptr_t local = fn_FindPlayerPed ? fn_FindPlayerPed(0) : 0;
    if (!local) return;
    
    Vec3 local_pos = get_ped_pos(local);
    auto peds = get_all_peds();

    // 1. Logica do Aimbot
    if (g_aimbot_enabled) {
        uintptr_t best_ped = 0;
        float best_dist = g_aimbot_range;
        Vec3 best_pos = {0,0,0};

        for (auto& [idx, ped] : peds) {
            if (ped == local || is_friend(idx)) continue;
            Vec3 pos = get_ped_pos(ped);
            float d = dist3d(local_pos, pos);
            if (d < best_dist) { best_dist = d; best_ped = ped; best_pos = pos; }
        }
        if (best_ped) { g_aimbot_target = best_pos; g_has_target = true; }
        else { g_has_target = false; }
    } else {
        g_has_target = false;
    }

    // 2. Logica do ESP
    if (g_esp_enabled) {
        int off = 0;
        off += snprintf(g_esp_buf + off, sizeof(g_esp_buf) - off, "[");
        bool first = true;
        for (auto& [idx, ped] : peds) {
            if (ped == local) continue;
            Vec3 pos = get_ped_pos(ped);
            float hp = get_ped_health(ped);
            float d = dist3d(local_pos, pos);
            bool fri = is_friend(idx);
            if (!first) off += snprintf(g_esp_buf+off, sizeof(g_esp_buf)-off, ",");
            off += snprintf(g_esp_buf+off, sizeof(g_esp_buf)-off,
                "{\"id\":%d,\"hp\":%.0f,\"dist\":%.0f,\"friend\":%s,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}",
                idx, hp, d, fri?"true":"false", pos.x, pos.y, pos.z);
            first = false;
        }
        snprintf(g_esp_buf+off, sizeof(g_esp_buf)-off, "]");
    } else {
        g_esp_buf[0] = '['; g_esp_buf[1] = ']'; g_esp_buf[2] = 0;
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_brp_mod_ModMenu_getAimbotTarget(JNIEnv* env, jclass) {
    jfloatArray arr = env->NewFloatArray(4);
    float buf[4] = { g_aimbot_target.x, g_aimbot_target.y, g_aimbot_target.z, g_has_target ? 1.f : 0.f };
    env->SetFloatArrayRegion(arr, 0, 4, buf);
    return arr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_brp_mod_ModMenu_getESPJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(g_esp_buf);
}

static void* mod_thread(void*) {
    sleep(5); // Espera o jogo carregar
    init_funcs();
    while (true) {
        mod_thread_tick();
        usleep(16666);
    }
    return nullptr;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    pthread_t tid;
    pthread_create(&tid, nullptr, mod_thread, nullptr);
    pthread_detach(tid);
    LOGI("BRP Mod carregado (32-bit)");
    return JNI_VERSION_1_6;
}
