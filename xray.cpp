// XrayLevi v0.1 - X-Ray de minerales para LeviLauncher (Minecraft Bedrock, Android arm64).
// Autor: erickzgcs
//
// Cómo funciona: cada tick del juego lee, poco a poco, los bloques cercanos al jugador (solo los que
// el cliente ya tiene cargados), detecta minerales por el nombre del bloque y dibuja un contorno de
// color alrededor de cada uno. No pide chunks ni manda paquetes al servidor.
//
// Firmas, offsets y método de dibujo tomados de BedrockTools (MIT License, Copyright (c) 2026
// RadiantByte, https://github.com/QYCottage/BedrockTools), versión para Minecraft 1.26.6X.X.
// Los offsets y firmas cambian con cada versión del juego: si Minecraft se actualiza, hay que
// revalidarlos.
//
// Úsalo en tu propio mundo. En servidores es trampa y los anticheats pueden banearte.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <android/log.h>
#include <dlfcn.h>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kTag = "XrayLevi";
constexpr const char* kModuleId = "xraylevi.main";
constexpr const char* kButtonId = "xraylevi.toggle";
constexpr const char* kLibrary = "libminecraftpe.so";

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)

struct BlockPos {
    int x, y, z;
};
struct Vec3f {
    float x, y, z;
};

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);
typedef void* (*BlockSource_getBlock_t)(void* region, const BlockPos& pos);

Tessellator_begin_t s_tessBegin = nullptr;
Tessellator_color_t s_tessColor = nullptr;
Tessellator_vertex_t s_tessVertex = nullptr;
MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
BlockSource_getBlock_t s_getBlock = nullptr;

// ---- Ayudantes tomados de BedrockTools (MIT) ----
struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}
    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }
private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

MaterialPtr s_matSelection;
uintptr_t s_renderMaterialGroup = 0;

// ---- Offsets (Minecraft Android 1.26.6X.X, de BedrockTools 1.5.12) ----
constexpr std::size_t kActorDimension = 0x1E0;
constexpr std::size_t kActorStateVector = 0x228;
constexpr std::size_t kDimensionBlockSource = 208;
constexpr std::size_t kBlockType = 0x68;
constexpr std::size_t kBlockTypeNameInfo = 0x88;
constexpr std::size_t kNameInfoFullName = 0x40;
constexpr std::size_t kHashedStringStr = 0x8;
constexpr std::size_t kLevelRendererPlayer = 0x430;
constexpr std::size_t kCamPos = 0x614;
constexpr std::size_t kSelectionOverlayMaterial = 0x10A0;
constexpr std::size_t kScreenTessellator = 0xB0;
constexpr std::size_t kScreenColorHolder = 0x30;
constexpr std::size_t kShaderColorDirty = 0x10;
constexpr std::size_t kMaterialGroupOffset = 32;

// ---- Firmas (BedrockTools 1.5.12, 1.26.6X.X) ----
struct SigDef {
    const char* name;
    const char* pattern;
};
const SigDef kSigNormalTick = {"NormalTick", "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39"};
const SigDef kSigRenderLevel = {"RenderLevel", "? ? ? D1 ? ? ? FD ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 57 D0 3B D5"};
const SigDef kSigTessBegin = {"TessellatorBegin", "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? 39 ? ? ? 39 08 01 09 2A"};
const SigDef kSigTessColor = {"TessellatorColor", "? ? ? 52 ? ? ? 39 04 01 27 1E 00 08 24 1E 21 08 24 1E 42 08 24 1E 63 08 24 1E 08 00 38 1E"};
const SigDef kSigTessVertex = {"TessellatorVertex", "? ? ? D1 ? ? ? FD ? ? ? 6D ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5B D0 3B D5 ? ? ? F9 ? ? ? F9"};
const SigDef kSigRenderMesh2 = {"RenderMeshImmediately2", "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 57 D0 3B D5 F6 03 00 AA E0 03 01 AA ? ? ? F9 F4 03 03 AA"};
const SigDef kSigRenderMesh1 = {"RenderMeshImmediately", "? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 58 D0 3B D5 F7 03 00 AA E0 03 01 AA ? ? ? F9 F4 03 04 AA"};
const SigDef kSigMaterialGroup = {"RenderMaterialGroupCommon", "? ? ? ? ? ? ? 91 ? ? ? D1 ? ? ? D1 ? ? ? 91 E3 03 14 AA ? ? ? 52 ? ? ? ? ? ? ? F9 ? ? ? A9 ? ? ? B4 ? ? ? 91 ? ? ? 92 ? ? ? ? ? ? ? B5 ? ? ? F9 E0 03 14 AA ? ? ? F9 00 01 3F D6 E0 03 14 AA ? ? ? ? ? ? ? F9 ? ? ? F9 ? ? ? B4 ? ? ? 91 ? ? ? 92 ? ? ? ? ? ? ? B5 ? ? ? F9 E0 03 14 AA ? ? ? F9 00 01 3F D6 E0 03 14 AA ? ? ? ? ? ? ? 38 ? ? ? 37 ? ? ? 39 ? ? ? 37 ? ? ? F8 ? ? ? A9 ? ? ? B4 ? ? ? 91 ? ? ? 92 ? ? ? ? ? ? ? B5 ? ? ? F9 E0 03 14 AA ? ? ? F9 00 01 3F D6 E0 03 14 AA ? ? ? ? ? ? ? F8 ? ? ? F8 ? ? ? B4 ? ? ? 91 ? ? ? 92 ? ? ? ? ? ? ? B5 ? ? ? F9 E0 03 14 AA ? ? ? F9 00 01 3F D6 E0 03 14 AA ? ? ? ? ? ? ? ? ? ? ? F9 ? ? ? 38 ? ? ? 36 ? ? ? F8 ? ? ? ? ? ? ? 39 ? ? ? 36 ? ? ? F9 ? ? ? ? ? ? ? F8 ? ? ? A9 ? ? ? B5 ? ? ? F8 ? ? ? F9 ? ? ? A9 ? ? ? F9 ? ? ? F9 ? ? ? B4 ? ? ? A9 ? ? ? B4 ? ? ? 91 ? ? ? 52 ? ? ? ? ? ? ? F9 ? ? ? F9 ? ? ? B4 ? ? ? F9 ? ? ? 91 E1 03 13 AA ? ? ? 91 ? ? ? ? ? ? ? 39 ? ? ? A9 ? ? ? 72 ? ? ? D3 29 15 94 9A 08 01 8A 9A ? ? ? A9 ? ? ? D1 ? ? ? 91 ? ? ? ? ? ? ? F9 ? ? ? F9 ? ? ? F9 E8 03 00 91"};
const SigDef kSigGetBlock = {"BlockSourceGetBlock", "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 56 D0 3B D5 ? ? ? F9 ? ? ? F9 ? ? ? B9 ? ? ? 79 1F 01 09 6B ? ? ? 54 ? ? ? 79 F3 03 00 AA 1F 01 09 6B ? ? ? 54 E0 03 00 91"};

// ---- Minerales ----
enum Ore { Diamond, Emerald, Gold, Iron, Copper, Redstone, Lapis, Coal, Quartz, Debris, OreCount };

struct OreInfo {
    const char* key;
    const char* label;
    uint32_t defaultColor;  // ARGB
};
constexpr OreInfo kOres[OreCount] = {
    {"diamond", "Diamante", 0xFF32D9FF}, {"emerald", "Esmeralda", 0xFF35E77B}, {"gold", "Oro", 0xFFFFD34F},
    {"iron", "Hierro", 0xFFD4D9E2},      {"copper", "Cobre", 0xFFFF925C},      {"redstone", "Redstone", 0xFFFF4D5D},
    {"lapis", "Lapislázuli", 0xFF527BFF}, {"coal", "Carbón", 0xFFB5B9C2},      {"quartz", "Cuarzo del Nether", 0xFFFFF0D5},
    {"debris", "Escombros antiguos", 0xFFB8795E},
};

struct Config {
    std::atomic<bool> enabled{false};
    std::atomic<int> radius{16};
    std::atomic<int> vertical{24};
    std::atomic<int> perTick{3000};
    std::atomic<int> maxMarkers{2048};
    std::atomic<bool> oreOn[OreCount];
    std::atomic<uint32_t> color[OreCount];
    Config() {
        for (int i = 0; i < OreCount; ++i) {
            oreOn[i] = true;
            color[i] = kOres[i].defaultColor;
        }
    }
};
Config C;

struct Marker {
    int x, y, z;
    int ore;
};

std::mutex gMutex;
std::vector<Marker> gMarkers;  // lo que se dibuja (publicado al terminar cada ciclo)
std::filesystem::path gConfigFile;
std::string gStatus = "Sin iniciar";
std::atomic<bool> gSettingUi{false};

// ---- Estado del escaneo (solo lo toca el hilo del tick) ----
std::vector<Marker> gPending;
std::unordered_map<void*, int> gKind;  // Block* -> mineral (-1 = no es mineral)
uintptr_t gLastBlockSource = 0;
long gIdx = 0, gTotal = 0;
int gCx = 0, gCy = 0, gCz = 0, gR = 0, gV = 0;
bool gScanning = false;
bool gLoggedFirst = false;
int64_t gLastLogNs = 0;

// ---- Hooks ----
using TickFn = void (*)(void*);
using RenderLevelFn = void (*)(void*, void*, void*);
TickFn gTickOrig = nullptr;
RenderLevelFn gRenderOrig = nullptr;
void* gTickTarget = nullptr;
void* gRenderTarget = nullptr;
bool gHooked = false;

int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::string blockName(void* block) {
    if (!block) return {};
    const uintptr_t type = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(block) + kBlockType);
    if (type < 0x1000) return {};
    const auto* s = reinterpret_cast<const std::string*>(type + kBlockTypeNameInfo + kNameInfoFullName + kHashedStringStr);
    const std::size_t n = s->size();
    if (n == 0 || n > 96) return {};
    return std::string(s->data(), n);
}

int classify(const std::string& name) {
    if (name.empty()) return -1;
    if (name.find("ancient_debris") != std::string::npos) return Debris;
    if (name.find("_ore") == std::string::npos) return -1;
    static const struct {
        const char* word;
        int ore;
    } table[] = {{"diamond", Diamond}, {"emerald", Emerald}, {"gold", Gold},   {"iron", Iron},     {"copper", Copper},
                 {"redstone", Redstone}, {"lapis", Lapis},   {"coal", Coal},   {"quartz", Quartz}};
    for (const auto& t : table)
        if (name.find(t.word) != std::string::npos) return t.ore;
    return -1;
}

// ------------------------------------------------------------------------------------------
// Persistencia
// ------------------------------------------------------------------------------------------
std::string colorToHex(uint32_t c) {
    char buf[12];
    snprintf(buf, sizeof(buf), "#%08X", c);
    return buf;
}

uint32_t parseColor(std::string_view v, uint32_t fallback) {
    std::string s(v);
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return fallback;
    char* end = nullptr;
    unsigned long x = std::strtoul(s.c_str(), &end, 16);
    if (!end || *end != '\0') return fallback;
    if (s.size() == 6) x |= 0xFF000000UL;
    return static_cast<uint32_t>(x);
}

bool parseBool(std::string_view v) { return v == "true" || v == "1" || v == "on"; }

int parseInt(std::string_view v, int fallback) {
    std::string s(v);
    char* end = nullptr;
    long x = std::strtol(s.c_str(), &end, 10);
    return (end && end != s.c_str()) ? static_cast<int>(x) : fallback;
}

void saveConfig() {
    if (gConfigFile.empty()) return;
    nlohmann::json j = {
        {"radius", C.radius.load()},
        {"vertical", C.vertical.load()},
        {"perTick", C.perTick.load()},
        {"maxMarkers", C.maxMarkers.load()},
    };
    for (int i = 0; i < OreCount; ++i) {
        j[std::string("ore_") + kOres[i].key] = C.oreOn[i].load();
        j[std::string("color_") + kOres[i].key] = colorToHex(C.color[i].load());
    }
    if (std::ofstream o{gConfigFile}) o << j.dump(2) << "\n";
}

void loadConfig(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    gConfigFile = dir / "xraylevi.json";
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in{gConfigFile}) {
        j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
    }
    C.radius = std::clamp(j.value("radius", 16), 4, 32);
    C.vertical = std::clamp(j.value("vertical", 24), 8, 64);
    C.perTick = std::clamp(j.value("perTick", 3000), 500, 8000);
    C.maxMarkers = std::clamp(j.value("maxMarkers", 2048), 16, 4096);
    for (int i = 0; i < OreCount; ++i) {
        C.oreOn[i] = j.value(std::string("ore_") + kOres[i].key, true);
        C.color[i] = parseColor(j.value(std::string("color_") + kOres[i].key, colorToHex(kOres[i].defaultColor)),
                                kOres[i].defaultColor);
    }
    saveConfig();
}

// ------------------------------------------------------------------------------------------
// Escaneo (se ejecuta en el tick del juego, en pedacitos)
// ------------------------------------------------------------------------------------------
void startCycle(const Vec3f& p) {
    gCx = static_cast<int>(std::floor(p.x));
    gCy = static_cast<int>(std::floor(p.y));
    gCz = static_cast<int>(std::floor(p.z));
    gR = C.radius.load();
    gV = C.vertical.load();
    const long d = 2L * gR + 1;
    gTotal = d * (2L * gV + 1) * d;
    gIdx = 0;
    gPending.clear();
    gScanning = true;
}

void scanTick(void* player) {
    if (!s_getBlock) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(player);
    const uintptr_t dim = *reinterpret_cast<uintptr_t*>(base + kActorDimension);
    if (dim < 0x1000) return;
    const uintptr_t bs = *reinterpret_cast<uintptr_t*>(dim + kDimensionBlockSource);
    if (bs < 0x1000) return;
    const uintptr_t svc = *reinterpret_cast<uintptr_t*>(base + kActorStateVector);
    if (svc < 0x1000) return;
    const Vec3f p = *reinterpret_cast<Vec3f*>(svc);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;

    if (bs != gLastBlockSource) {  // otro mundo o dimensión: los punteros viejos ya no sirven
        gLastBlockSource = bs;
        gKind.clear();
        gScanning = false;
        std::lock_guard<std::mutex> lock(gMutex);
        gMarkers.clear();
    }

    if (!gLoggedFirst) {  // etapa 1: comprobar que la lectura de bloques funciona
        gLoggedFirst = true;
        const BlockPos under{static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)) - 1,
                             static_cast<int>(std::floor(p.z))};
        const std::string name = blockName(s_getBlock(reinterpret_cast<void*>(bs), under));
        LOGI("Bloque bajo tus pies: %s", name.empty() ? "(no se pudo leer)" : name.c_str());
        gStatus = name.empty() ? "Lectura de bloques: FALLÓ" : "Lectura de bloques OK (" + name + ")";
    }

    if (!gScanning) startCycle(p);

    const long d = 2L * gR + 1;
    const int maxMarkers = C.maxMarkers.load();
    int budget = C.perTick.load();
    for (; budget > 0 && gIdx < gTotal; --budget, ++gIdx) {
        const int ix = static_cast<int>(gIdx % d);
        const long rest = gIdx / d;
        const int iz = static_cast<int>(rest % d);
        const int iy = static_cast<int>(rest / d);
        const BlockPos bp{gCx - gR + ix, gCy - gV + iy, gCz - gR + iz};
        if (bp.y < -64 || bp.y > 320) continue;
        void* blk = s_getBlock(reinterpret_cast<void*>(bs), bp);
        if (!blk) continue;
        int kind;
        auto it = gKind.find(blk);
        if (it == gKind.end()) {
            kind = classify(blockName(blk));
            gKind.emplace(blk, kind);
        } else {
            kind = it->second;
        }
        if (kind >= 0 && C.oreOn[kind].load() && static_cast<int>(gPending.size()) < maxMarkers)
            gPending.push_back({bp.x, bp.y, bp.z, kind});
    }

    if (gIdx >= gTotal) {  // ciclo completo: publicar y empezar otro
        const std::size_t n = gPending.size();
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gMarkers.swap(gPending);
        }
        gPending.clear();
        gScanning = false;
        const int64_t now = nowNs();
        if (now - gLastLogNs > 10'000'000'000LL) {
            gLastLogNs = now;
            LOGI("Ciclo de escaneo completo: %zu minerales (radio %d, vertical %d, bloques en cache %zu)", n, gR, gV,
                 gKind.size());
        }
    }
}

void tickDetour(void* actor) {
    if (gTickOrig) gTickOrig(actor);
    if (!C.enabled.load() || !actor) return;
    scanTick(actor);
}

// ------------------------------------------------------------------------------------------
// Dibujo
// ------------------------------------------------------------------------------------------
MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};
    using getMat_t = MaterialPtr (*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])(reinterpret_cast<void*>(s_renderMaterialGroup), &hs);
}

void ensureMaterial() {
    if (s_matSelection || !s_renderMaterialGroup) return;
    s_matSelection = getMaterial("selection_box");
}

class ScopedShaderColor {
public:
    explicit ScopedShaderColor(void* screenContext) {
        if (!screenContext) return;
        mHolder = *reinterpret_cast<std::byte**>(static_cast<std::byte*>(screenContext) + kScreenColorHolder);
        if (!mHolder) return;
        std::memcpy(mSaved, mHolder, sizeof(mSaved));
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(mHolder, white, sizeof(white));
        mHolder[kShaderColorDirty] = std::byte{1};
    }
    ~ScopedShaderColor() {
        if (!mHolder) return;
        std::memcpy(mHolder, mSaved, sizeof(mSaved));
        mHolder[kShaderColorDirty] = std::byte{1};
    }
    explicit operator bool() const { return mHolder != nullptr; }
    ScopedShaderColor(const ScopedShaderColor&) = delete;
    ScopedShaderColor& operator=(const ScopedShaderColor&) = delete;

private:
    std::byte* mHolder = nullptr;
    float mSaved[4]{};
};

void renderDetour(void* self, void* screenContext, void* a3) {
    if (gRenderOrig) gRenderOrig(self, screenContext, a3);

    if (!C.enabled.load()) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000) return;

    const uintptr_t tessPtr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(screenContext) + kScreenTessellator);
    if (tessPtr < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessPtr);

    const uintptr_t lrp = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(self) + kLevelRendererPlayer);
    if (lrp < 0x1000) return;
    const float camX = *reinterpret_cast<float*>(lrp + kCamPos);
    const float camY = *reinterpret_cast<float*>(lrp + kCamPos + 4);
    const float camZ = *reinterpret_cast<float*>(lrp + kCamPos + 8);

    ensureMaterial();
    void* material = s_matSelection ? static_cast<void*>(&s_matSelection) : reinterpret_cast<void*>(lrp + kSelectionOverlayMaterial);

    ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    std::vector<Marker> local;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        local = gMarkers;
    }
    if (local.empty()) return;

    static const int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                      {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    constexpr float e = 0.004f;

    for (int ore = 0; ore < OreCount; ++ore) {
        int count = 0;
        for (const auto& m : local)
            if (m.ore == ore) ++count;
        if (count == 0) continue;

        const uint32_t col = C.color[ore].load();
        const float a = ((col >> 24) & 0xFF) / 255.0f;
        const float r = ((col >> 16) & 0xFF) / 255.0f;
        const float g = ((col >> 8) & 0xFF) / 255.0f;
        const float b = (col & 0xFF) / 255.0f;

        s_tessBegin(tessellator, nullptr, 4, count * 24, 0);  // 4 = lista de líneas
        s_tessColor(tessellator, r, g, b, a);
        for (const auto& m : local) {
            if (m.ore != ore) continue;
            const float x0 = m.x - e - camX, x1 = m.x + 1 + e - camX;
            const float y0 = m.y - e - camY, y1 = m.y + 1 + e - camY;
            const float z0 = m.z - e - camZ, z1 = m.z + 1 + e - camZ;
            const float cx[8] = {x0, x1, x0, x1, x0, x1, x0, x1};
            const float cy[8] = {y0, y0, y0, y0, y1, y1, y1, y1};
            const float cz[8] = {z0, z0, z1, z1, z0, z0, z1, z1};
            for (const auto& edge : kEdges) {
                s_tessVertex(tessellator, cx[edge[0]], cy[edge[0]], cz[edge[0]]);
                s_tessVertex(tessellator, cx[edge[1]], cy[edge[1]], cz[edge[1]]);
            }
        }
        char pad[0x58];
        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, material, pad);
    }
}

// ------------------------------------------------------------------------------------------
// Instalación de hooks
// ------------------------------------------------------------------------------------------
bool installHooks() {
    if (gHooked) return true;

    const SigDef* defs[] = {&kSigNormalTick, &kSigRenderLevel, &kSigTessBegin,   &kSigTessColor, &kSigTessVertex,
                            &kSigRenderMesh2, &kSigRenderMesh1, &kSigMaterialGroup, &kSigGetBlock};
    std::vector<std::string> patterns;
    for (const auto* d : defs) patterns.emplace_back(d->pattern);
    const auto found = pl::memory::resolveSignatures(patterns, kLibrary);

    auto get = [&](const SigDef& d) -> uintptr_t {
        auto it = found.find(d.pattern);
        return it == found.end() ? 0 : it->second;
    };

    const uintptr_t tick = get(kSigNormalTick), render = get(kSigRenderLevel);
    const uintptr_t tb = get(kSigTessBegin), tc = get(kSigTessColor), tv = get(kSigTessVertex);
    uintptr_t rm = get(kSigRenderMesh2);
    if (!rm) rm = get(kSigRenderMesh1);
    const uintptr_t rmg = get(kSigMaterialGroup), gb = get(kSigGetBlock);

    std::string missing;
    auto need = [&](uintptr_t v, const char* n) {
        if (!v) missing += std::string(missing.empty() ? "" : ", ") + n;
    };
    need(tick, "NormalTick");
    need(render, "RenderLevel");
    need(tb, "TessellatorBegin");
    need(tc, "TessellatorColor");
    need(tv, "TessellatorVertex");
    need(rm, "RenderMeshImmediately");
    need(rmg, "RenderMaterialGroup");
    need(gb, "BlockSourceGetBlock");
    if (!missing.empty()) {
        gStatus = "Versión de Minecraft no compatible. Faltan firmas: " + missing;
        LOGI("%s", gStatus.c_str());
        return false;
    }

    s_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb);
    s_tessColor = reinterpret_cast<Tessellator_color_t>(tc);
    s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv);
    s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    s_getBlock = reinterpret_cast<BlockSource_getBlock_t>(gb);

    const uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
    if (groupAddr) s_renderMaterialGroup = groupAddr + kMaterialGroupOffset;

    gTickTarget = reinterpret_cast<void*>(tick);
    gRenderTarget = reinterpret_cast<void*>(render);
    if (pl::memory::hook(gTickTarget, reinterpret_cast<void*>(tickDetour), reinterpret_cast<void**>(&gTickOrig)) != 0) {
        gStatus = "No se pudo enganchar el tick del jugador";
        return false;
    }
    if (pl::memory::hook(gRenderTarget, reinterpret_cast<void*>(renderDetour), reinterpret_cast<void**>(&gRenderOrig)) != 0) {
        pl::memory::unhook(gTickTarget, reinterpret_cast<void*>(tickDetour));
        gStatus = "No se pudo enganchar el render";
        return false;
    }
    gHooked = true;
    gStatus = "Listo. Activa el X-Ray con el botón o el interruptor del módulo.";
    LOGI("Hooks instalados");
    return true;
}

void removeHooks() {
    if (!gHooked) return;
    pl::memory::unhook(gTickTarget, reinterpret_cast<void*>(tickDetour));
    pl::memory::unhook(gRenderTarget, reinterpret_cast<void*>(renderDetour));
    gHooked = false;
    C.enabled = false;
    std::lock_guard<std::mutex> lock(gMutex);
    gMarkers.clear();
}

// ------------------------------------------------------------------------------------------
// Menú y botón flotante
// ------------------------------------------------------------------------------------------
using namespace pl::modmenu;

constexpr const char* kSvgOff = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
<rect x="4" y="4" width="56" height="56" rx="6" fill="#2B2B2B" stroke="#555" stroke-width="3"/>
<path d="M32 12 L50 28 L32 52 L14 28 Z" fill="none" stroke="#8A8A8A" stroke-width="4" stroke-linejoin="round"/>
<path d="M14 28 H50 M24 18 L32 28 L40 18 M32 28 V52" fill="none" stroke="#8A8A8A" stroke-width="2"/>
</svg>)svg";

constexpr const char* kSvgOn = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
<rect x="4" y="4" width="56" height="56" rx="6" fill="#0E2A33" stroke="#32D9FF" stroke-width="3"/>
<path d="M32 12 L50 28 L32 52 L14 28 Z" fill="#32D9FF" fill-opacity="0.25" stroke="#32D9FF" stroke-width="4" stroke-linejoin="round"/>
<path d="M14 28 H50 M24 18 L32 28 L40 18 M32 28 V52" fill="none" stroke="#32D9FF" stroke-width="2"/>
</svg>)svg";

ConfigNodeV2 baseNode(const std::string& key, const std::string& title, const std::string& desc, ConfigControlTypeV2 type,
                      const std::string& category) {
    ConfigNodeV2 n;
    n.id = key;
    n.key = key;
    n.title = title;
    n.description = desc;
    n.type = type;
    n.category = category;
    return n;
}

ConfigNodeV2 sliderNode(const std::string& key, const std::string& title, const std::string& desc, int cur, int mn, int mx,
                        int step, const std::string& unit, const std::string& cat) {
    auto n = baseNode(key, title, desc, ConfigControlTypeV2::SliderInt, cat);
    n.defaultValue = n.currentValue = std::to_string(cur);
    n.hasCurrentValue = true;
    n.minValue = std::to_string(mn);
    n.maxValue = std::to_string(mx);
    n.step = std::to_string(step);
    n.unit = unit;
    return n;
}

ConfigNodeV2 toggleNode(const std::string& key, const std::string& title, bool cur, const std::string& cat) {
    auto n = baseNode(key, title, "", ConfigControlTypeV2::Toggle, cat);
    n.defaultValue = n.currentValue = cur ? "true" : "false";
    n.hasCurrentValue = true;
    return n;
}

ConfigNodeV2 colorNode(const std::string& key, const std::string& title, uint32_t cur, const std::string& cat) {
    auto n = baseNode(key, title, "", ConfigControlTypeV2::Color, cat);
    n.defaultValue = n.currentValue = colorToHex(cur);
    n.hasCurrentValue = true;
    return n;
}

std::string buildSchema() {
    ConfigSchemaBuilder b;
    b.category("general", "General", "Alcance del escaneo")
        .category("minerales", "Minerales", "Qué minerales ver y de qué color")
        .category("info", "Estado", "Diagnóstico")
        .defaultCategory("general");

    b.node(sliderNode("radius", "Radio horizontal", "Más grande = más lejos, pero más trabajo para el celular", C.radius.load(),
                      4, 32, 1, "bloques", "general"));
    b.node(sliderNode("vertical", "Rango vertical", "Bloques hacia arriba y abajo desde tu altura", C.vertical.load(), 8, 64,
                      2, "bloques", "general"));
    b.node(sliderNode("perTick", "Bloques por tick", "Más alto = escaneo más rápido, pero puede dar tirones",
                      C.perTick.load(), 500, 8000, 250, "", "general"));
    b.node(sliderNode("maxMarkers", "Máximo de minerales dibujados", "", C.maxMarkers.load(), 16, 4096, 16, "", "general"));

    for (int i = 0; i < OreCount; ++i) {
        b.node(toggleNode(std::string("ore_") + kOres[i].key, std::string("Mostrar ") + kOres[i].label, C.oreOn[i].load(),
                          "minerales"));
        b.node(colorNode(std::string("color_") + kOres[i].key, std::string("Color: ") + kOres[i].label, C.color[i].load(),
                         "minerales"));
    }

    b.node(baseNode("status", "Estado", gStatus, ConfigControlTypeV2::Info, "info"));
    b.node(baseNode("note", "Importante",
                    "Solo muestra minerales de chunks ya cargados. Úsalo en tu mundo, no en servidores.",
                    ConfigControlTypeV2::Info, "info"));
    return b.toJson();
}

void pushSchema() {
    gSettingUi = true;
    setConfigSchemaJson(kModuleId, buildSchema());
    gSettingUi = false;
}

void setEnabled(bool on) {
    if (on && !gHooked) return;
    C.enabled = on;
    if (!on) {
        std::lock_guard<std::mutex> lock(gMutex);
        gMarkers.clear();
        gScanning = false;
    }
}

void onConfigChanged(std::string_view, std::string_view key, std::string_view value) {
    if (gSettingUi.load()) return;
    if (key == "radius") C.radius = std::clamp(parseInt(value, 16), 4, 32);
    else if (key == "vertical") C.vertical = std::clamp(parseInt(value, 24), 8, 64);
    else if (key == "perTick") C.perTick = std::clamp(parseInt(value, 3000), 500, 8000);
    else if (key == "maxMarkers") C.maxMarkers = std::clamp(parseInt(value, 2048), 16, 4096);
    else {
        bool done = false;
        for (int i = 0; i < OreCount && !done; ++i) {
            if (key == std::string("ore_") + kOres[i].key) {
                C.oreOn[i] = parseBool(value);
                done = true;
            } else if (key == std::string("color_") + kOres[i].key) {
                C.color[i] = parseColor(value, kOres[i].defaultColor);
                done = true;
            }
        }
        if (!done) return;
    }
    saveConfig();
}

bool registerMenu(const std::string& modId) {
    ModuleBuilder builder(kModuleId, "XrayLevi");
    builder.description("X-Ray de minerales - por erickzgcs")
        .modId(modId)
        .defaultEnabled(false)
        .onToggle([](std::string_view, bool enabled) { setEnabled(enabled); })
        .onConfigChanged(onConfigChanged);
    if (!builder.registerModule()) return false;
    pushSchema();

    const bool button = ButtonBuilder(kButtonId, "X-Ray")
                            .moduleId(kModuleId)
                            .modId(modId)
                            .behavior(ButtonBehavior::Toggle)
                            .stylePreset(ButtonStylePreset::Accent)
                            .styleColors(0x00000001, 0x00000001, 0x00000001)
                            .svgIcon(kSvgOff)
                            .activeSvgIcon(kSvgOn)
                            .onEvent([](std::string_view, ButtonEvent event, float value) {
                                if (event == ButtonEvent::StateChanged) {
                                    const bool on = value > 0.5f;
                                    setEnabled(on);
                                    setModuleEnabled(kModuleId, C.enabled.load());
                                }
                            })
                            .registerButton();
    if (!button) LOGI("No se pudo registrar el botón flotante");
    return true;
}

}  // namespace

class XrayLeviMod {
public:
    static XrayLeviMod& instance() {
        static XrayLeviMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) {
        loadConfig(context.configDir());
        context.logger().info("XrayLevi 0.1 cargado");
        return true;
    }

    bool enable(pl::mod::ModContext& context) {
        const bool ok = installHooks();
        if (!ok) context.logger().info("XrayLevi: {}", gStatus);
        registerMenu(context.id());  // el menú se registra aunque falten firmas, para mostrar el estado
        return true;
    }

    bool disable(pl::mod::ModContext&) {
        unregisterButton(kButtonId);
        unregisterModule(kModuleId);
        removeHooks();
        return true;
    }

    bool unload(pl::mod::ModContext&) { return true; }
};

PL_REGISTER_MOD(XrayLeviMod, XrayLeviMod::instance())
