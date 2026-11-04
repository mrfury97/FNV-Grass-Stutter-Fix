// Grass Stutter Fix - NVSE
//
// FalloutNV.exe 1.4.0.525 rebuilds grass on the main thread. Facts from the decompiled executable:
//   - PlayerCharacter::UpdateCamera calls TESTerrainLODManager::UpdateGrass (call at 0x94C130) after a 64-unit move OR a
//     small view turn. UpdateGrassForCell (0x57D4F0) then DELETES every land quad that is outside the (widened) frustum
//     (jz at 0x57D93B -> RemoveFromTerrainChunk) and recreates quads that came back into view with CreateGrass, so
//     spinning in place deletes and rebuilds grass all the time.
//   - A (patch, grass type) that produced no instance is never marked in the type's chunk map, so CreateGrass runs again
//     for it on every update (ContainsChunk call at 0x57DAD4).
//   - Everything new is created in the same frame.
// What this plugin changes (each part can be switched off in GrassStutterFix.ini):
//   bKeepOffscreen  : NOP the frustum jump, quads in range are kept (and built) whether in view or not. Drawing is still
//                     frustum-culled by the scene graph, so only memory goes up.
//   bSkipTurnOnly   : with bKeepOffscreen, an update caused only by turning has nothing to do -> skipped.
//   bMemoEmpty      : remember (patch, type) pairs whose CreateGrass gave nothing (with the land loaded), so they are not
//                     rolled again until that cell/chunk is removed or the camera jumps cells.
//   iBudget         : during camera updates at most iBudget CreateGrass calls per frame for patches farther than
//                     fNearRadius; the rest is deferred and the plugin forces another update next frame.
// Log: logs\GrassStutterFix.log (slow updates and a summary every 10 s).
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <unordered_map>
#include <vector>

#include "nvse/nvse_version.h"
#include "nvse/PluginAPI.h"

static const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------------------------------------------
static FILE* gLog = nullptr;
static void Log(const char* fmt, ...) {
	if (!gLog) return;
	va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a);
	fputc('\n', gLog); fflush(gLog);
}
static LARGE_INTEGER gQpcFreq;
static double NowMs() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * 1000.0 / gQpcFreq.QuadPart; }

static void SafeWrite32(uintptr_t addr, uint32_t value) {
	DWORD old; VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old);
	*(uint32_t*)addr = value; VirtualProtect((void*)addr, 4, old, &old);
}
static uintptr_t ReplaceCall(uintptr_t at, void* fn, const char* what) {
	if (*(uint8_t*)at != 0xE8) { Log("  %s: no call at %08X (byte %02X) - not hooked", what, (unsigned)at, *(uint8_t*)at); return 0; }
	const uintptr_t previous = at + 5 + *(int32_t*)(at + 1);
	SafeWrite32(at + 1, (uintptr_t)fn - at - 5);
	Log("  %s: call at %08X now ours (was -> %08X)", what, (unsigned)at, (unsigned)previous);
	return previous;
}
static bool PatchBytes(uintptr_t at, const uint8_t* expect, const uint8_t* repl, size_t n, const char* what) {
	if (memcmp((void*)at, expect, n)) {
		Log("  %s: bytes at %08X differ from vanilla (another mod?) - not patched", what, (unsigned)at);
		return false;
	}
	DWORD old; VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)at, repl, n); VirtualProtect((void*)at, n, old, &old);
	Log("  %s: patched %u bytes at %08X", what, (unsigned)n, (unsigned)at);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings
static struct {
	bool keepOffscreen = true, skipTurnOnly = true, memoEmpty = true, log = true;
	int budget = 150;
	float nearRadius = 3000.f;
	float slowMs = 8.f;
} gSet;

static void LoadSettings() {
	const char* ini = "Data\\NVSE\\Plugins\\GrassStutterFix.ini";
	gSet.keepOffscreen = GetPrivateProfileIntA("Main", "bKeepOffscreen", 1, ini) != 0;
	gSet.skipTurnOnly = GetPrivateProfileIntA("Main", "bSkipTurnOnly", 1, ini) != 0;
	gSet.memoEmpty = GetPrivateProfileIntA("Main", "bMemoEmpty", 1, ini) != 0;
	gSet.budget = GetPrivateProfileIntA("Main", "iBudget", 150, ini);
	gSet.log = GetPrivateProfileIntA("Main", "bLog", 1, ini) != 0;
	char b[32];
	GetPrivateProfileStringA("Main", "fNearRadius", "3000", b, sizeof(b), ini); gSet.nearRadius = (float)atof(b);
	GetPrivateProfileStringA("Main", "fSlowMs", "8", b, sizeof(b), ini); gSet.slowMs = (float)atof(b);
	Log("Settings: bKeepOffscreen=%d bSkipTurnOnly=%d bMemoEmpty=%d iBudget=%d fNearRadius=%.0f fSlowMs=%.1f",
		gSet.keepOffscreen, gSet.skipTurnOnly, gSet.memoEmpty, gSet.budget, gSet.nearRadius, gSet.slowMs);
}

// ---------------------------------------------------------------------------------------------------------------
// Engine
struct NiPoint3 { float x, y, z; };
static NiPoint3& kLastGrassUpdateDir = *(NiPoint3*)0x11E0C08;
static NiPoint3& kLastGrassUpdatePos = *(NiPoint3*)0x11E0C14;
typedef bool(__thiscall* LandIsLoaded_t)(void* land);
static const LandIsLoaded_t LandIsLoaded = (LandIsLoaded_t)0x5394A0;

typedef void(__cdecl* UpdateGrass_t)(NiPoint3 pos, NiPoint3 dir, float angle);
typedef bool(__cdecl* ContainsChunk_t)(uint32_t grassID, uint32_t chunk);
typedef void(__cdecl* CreateGrass_t)(int32_t cx, int32_t cy, uint32_t block, void* land, uint32_t chunk, NiPoint3* vpos,
	void* node, NiPoint3 cam, void* param, float* density, float dist, float endDist, uint32_t heightLimit, int32_t water,
	float cellWater, uint32_t minSlope, uint32_t maxSlope);
typedef void(__cdecl* RemoveFromCell_t)(int32_t x, int32_t y);
typedef void(__cdecl* RemoveFromChunk_t)(uint32_t chunk);
typedef void(__cdecl* RemoveAll_t)();

static UpdateGrass_t oUpdateGrassCam, oUpdateGrassOther[4];
static ContainsChunk_t oContains;
static CreateGrass_t oCreate;
static RemoveFromCell_t oRemoveCell;
static RemoveFromChunk_t oRemoveChunk;
static RemoveAll_t oRemoveAll[2];
static const ContainsChunk_t ContainsChunkRaw = (ContainsChunk_t)0xB617B0;

// ---------------------------------------------------------------------------------------------------------------
// Memo of empty (chunk, grass type) pairs
struct MemoEntry { int64_t cell; std::vector<uint32_t> ids; };
static std::unordered_map<uint32_t, MemoEntry> gMemo;                 // chunk -> types that gave nothing
static std::unordered_map<int64_t, std::vector<uint32_t>> gMemoCells;  // cell -> chunks in gMemo
static int64_t CellKey(int32_t x, int32_t y) { return ((int64_t)x << 32) | (uint32_t)y; }
static void MemoClear() { gMemo.clear(); gMemoCells.clear(); }
static bool MemoHas(uint32_t chunk, uint32_t id) {
	auto it = gMemo.find(chunk);
	if (it == gMemo.end()) return false;
	for (uint32_t i : it->second.ids) if (i == id) return true;
	return false;
}
static void MemoAdd(int32_t cx, int32_t cy, uint32_t chunk, uint32_t id) {
	auto it = gMemo.find(chunk);
	if (it == gMemo.end()) {
		it = gMemo.emplace(chunk, MemoEntry{ CellKey(cx, cy), {} }).first;
		gMemoCells[CellKey(cx, cy)].push_back(chunk);
	}
	it->second.ids.push_back(id);
}

// ---------------------------------------------------------------------------------------------------------------
// Per-update state and statistics
static bool gInCam = false, gPending = false, gHadCamUpdate = false;
static int gCreates = 0, gDeferred = 0, gMemoHits = 0;
static double gCreateMs = 0;
static int32_t gLastCellX = INT32_MIN, gLastCellY = INT32_MIN;
static struct { int updates = 0, skipped = 0, slow = 0, creates = 0, deferred = 0, memoHits = 0; double ms = 0, maxMs = 0, createMs = 0, t0 = 0; } gSum;

static void SummaryMaybe(double now) {
	if (!gSet.log) return;
	if (gSum.t0 == 0) { gSum.t0 = now; return; }
	if (now - gSum.t0 < 10000) return;
	if (gSum.updates || gSum.skipped)
		Log("[10s] updates=%d skipped(turn)=%d  mean=%.2f ms max=%.2f ms  CreateGrass: %d calls %.1f ms, deferred=%d, memo hits=%d, memo=%u chunks",
			gSum.updates, gSum.skipped, gSum.updates ? gSum.ms / gSum.updates : 0, gSum.maxMs, gSum.creates, gSum.createMs,
			gSum.deferred, gSum.memoHits, (unsigned)gMemo.size());
	gSum = {}; gSum.t0 = now;
}

static bool __cdecl Hook_Contains(uint32_t id, uint32_t chunk) {
	if (oContains(id, chunk)) return true;
	if (gSet.memoEmpty && MemoHas(chunk, id)) { gMemoHits++; return true; }
	return false;
}

static void __cdecl Hook_Create(int32_t cx, int32_t cy, uint32_t block, void* land, uint32_t chunk, NiPoint3* vpos, void* node,
	NiPoint3 cam, void* param, float* density, float dist, float endDist, uint32_t heightLimit, int32_t water, float cellWater,
	uint32_t minSlope, uint32_t maxSlope) {
	if (gInCam && gSet.budget > 0 && gCreates >= gSet.budget) {
		const float dx = vpos->x + 256.f - cam.x, dy = vpos->y + 256.f - cam.y;
		if (dx * dx + dy * dy > gSet.nearRadius * gSet.nearRadius) { gDeferred++; gPending = true; return; }
	}
	gCreates++;
	const double t = NowMs();
	oCreate(cx, cy, block, land, chunk, vpos, node, cam, param, density, dist, endDist, heightLimit, water, cellWater, minSlope, maxSlope);
	gCreateMs += NowMs() - t;
	if (gSet.memoEmpty && param) {
		const uint32_t id = *(uint32_t*)((char*)param + 4);    // TESGrassAreaParam.kGrassParam.iGrassID
		if (!ContainsChunkRaw(id, chunk) && land && LandIsLoaded(land)) MemoAdd(cx, cy, chunk, id);
	}
}

static void __cdecl Hook_RemoveCell(int32_t x, int32_t y) {
	auto it = gMemoCells.find(CellKey(x, y));
	if (it != gMemoCells.end()) { for (uint32_t c : it->second) gMemo.erase(c); gMemoCells.erase(it); }
	oRemoveCell(x, y);
}
static void __cdecl Hook_RemoveChunk(uint32_t chunk) { gMemo.erase(chunk); oRemoveChunk(chunk); }   // gMemoCells keeps a stale key: harmless
static void __cdecl Hook_RemoveAll0() { MemoClear(); oRemoveAll[0](); }
static void __cdecl Hook_RemoveAll1() { MemoClear(); oRemoveAll[1](); }

static void __cdecl Hook_UpdateGrassCam(NiPoint3 pos, NiPoint3 dir, float angle) {
	const float mx = pos.x - kLastGrassUpdatePos.x, my = pos.y - kLastGrassUpdatePos.y;
	const bool moved = mx * mx + my * my > 64.f * 64.f;
	const int32_t cx = (int32_t)floorf(pos.x / 4096.f), cy = (int32_t)floorf(pos.y / 4096.f);
	const bool sameCell = cx == gLastCellX && cy == gLastCellY;
	const double t0 = NowMs();
	if (gSet.keepOffscreen && gSet.skipTurnOnly && !moved && !gPending && sameCell && gHadCamUpdate) {
		gSum.skipped++; SummaryMaybe(t0); return;
	}
	if (gLastCellX != INT32_MIN && (abs(cx - gLastCellX) > 1 || abs(cy - gLastCellY) > 1)) MemoClear();
	gLastCellX = cx; gLastCellY = cy;
	const char* why = moved ? "move" : (gPending ? "budget" : (!sameCell ? "cell" : "turn"));
	gInCam = true; gPending = false; gCreates = gDeferred = gMemoHits = 0; gCreateMs = 0;
	oUpdateGrassCam(pos, dir, angle);
	gInCam = false; gHadCamUpdate = true;
	const double ms = NowMs() - t0;
	gSum.updates++; gSum.ms += ms; if (ms > gSum.maxMs) gSum.maxMs = ms;
	gSum.creates += gCreates; gSum.createMs += gCreateMs; gSum.deferred += gDeferred; gSum.memoHits += gMemoHits;
	if (gSet.log && ms > gSet.slowMs) {
		gSum.slow++;
		Log("slow update %.2f ms (%s): CreateGrass %d calls %.2f ms, rest %.2f ms, deferred %d, memo hits %d, memo %u chunks",
			ms, why, gCreates, gCreateMs, ms - gCreateMs, gDeferred, gMemoHits, (unsigned)gMemo.size());
	}
	SummaryMaybe(NowMs());
}

// load / teleport / ToggleGrass callers: full update, no budget, fresh memo
static void OtherUpdate(int i, NiPoint3 pos, NiPoint3 dir, float angle) {
	MemoClear(); gPending = false; gHadCamUpdate = false;
	const double t0 = NowMs();
	oUpdateGrassOther[i](pos, dir, angle);
	if (gSet.log) Log("full grass update (caller %d): %.1f ms", i, NowMs() - t0);
}
static void __cdecl Hook_UpdateOther0(NiPoint3 p, NiPoint3 d, float a) { OtherUpdate(0, p, d, a); }
static void __cdecl Hook_UpdateOther1(NiPoint3 p, NiPoint3 d, float a) { OtherUpdate(1, p, d, a); }
static void __cdecl Hook_UpdateOther2(NiPoint3 p, NiPoint3 d, float a) { OtherUpdate(2, p, d, a); }
static void __cdecl Hook_UpdateOther3(NiPoint3 p, NiPoint3 d, float a) { OtherUpdate(3, p, d, a); }

// ---------------------------------------------------------------------------------------------------------------
static void Install() {
	Log("Installing:");
	oUpdateGrassCam = (UpdateGrass_t)ReplaceCall(0x94C130, Hook_UpdateGrassCam, "UpdateGrass (camera)");
	const uintptr_t others[4] = { 0x451436, 0x454822, 0x858F18, 0x5B7170 };
	void* otherHooks[4] = { Hook_UpdateOther0, Hook_UpdateOther1, Hook_UpdateOther2, Hook_UpdateOther3 };
	for (int i = 0; i < 4; i++) oUpdateGrassOther[i] = (UpdateGrass_t)ReplaceCall(others[i], otherHooks[i], "UpdateGrass (load/teleport/toggle)");
	for (int i = 0; i < 4; i++) if (!oUpdateGrassOther[i]) { Log("  missing an UpdateGrass caller - plugin stays off"); return; }
	if (!oUpdateGrassCam) { Log("  plugin stays off"); return; }
	oCreate = (CreateGrass_t)ReplaceCall(0x57DC1D, Hook_Create, "CreateGrass");
	if (gSet.memoEmpty) {
		oContains = (ContainsChunk_t)ReplaceCall(0x57DAD4, Hook_Contains, "ContainsChunk");
		oRemoveCell = (RemoveFromCell_t)ReplaceCall(0x57D45F, Hook_RemoveCell, "RemoveFromCell");
		oRemoveChunk = (RemoveFromChunk_t)ReplaceCall(0x57DD28, Hook_RemoveChunk, "RemoveFromTerrainChunk");
		oRemoveAll[0] = (RemoveAll_t)ReplaceCall(0x57D10E, Hook_RemoveAll0, "RemoveAllGrass (UpdateGrass)");
		oRemoveAll[1] = (RemoveAll_t)ReplaceCall(0xB62A47, Hook_RemoveAll1, "RemoveAllGrass (ClearStaticMaps)");
		if (!oContains || !oRemoveCell || !oRemoveChunk || !oRemoveAll[0] || !oRemoveAll[1]) {
			Log("  memo hooks incomplete - memo off");   // the hooks that did go in stay pass-through
			gSet.memoEmpty = false;
			if (!oContains) oContains = ContainsChunkRaw;
		}
	}
	if (gSet.keepOffscreen) {
		static const uint8_t jz[6] = { 0x0F, 0x84, 0x07, 0x03, 0x00, 0x00 }, nop[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		if (!PatchBytes(0x57D93B, jz, nop, 6, "KeepOffscreen (UpdateGrassForCell frustum jump)")) gSet.keepOffscreen = false;
	}
	Log("Active: keepOffscreen=%d skipTurnOnly=%d memo=%d budget=%d", gSet.keepOffscreen, gSet.keepOffscreen && gSet.skipTurnOnly,
		gSet.memoEmpty, gSet.budget);
}

static void MessageHandler(NVSEMessagingInterface::Message* msg) {
	if (msg->type == NVSEMessagingInterface::kMessage_PostLoad) Install();
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop) {
		// deferred work left: zero the last grass direction so UpdateCamera's turn test fires next frame
		if (gPending) kLastGrassUpdateDir = { 0.f, 0.f, 0.f };
	}
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "Grass Stutter Fix";
	info->version = 1;
	return !nvse->isEditor && nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse) {
	CreateDirectoryA("logs", nullptr);
	gLog = _fsopen("logs\\GrassStutterFix.log", "w", _SH_DENYWR);
	QueryPerformanceFrequency(&gQpcFreq);
	Log("Grass Stutter Fix %s", kVersion);
	LoadSettings();
	auto* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	if (!messaging || !messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", MessageHandler))
		Log("No NVSE messaging: disabled");
	return true;
}

BOOL WINAPI DllMain(HANDLE, DWORD, LPVOID) { return TRUE; }
