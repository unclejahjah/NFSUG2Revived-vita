#ifndef WINAPI_D3D9_PROBE_H_
#define WINAPI_D3D9_PROBE_H_

// 4c wait-risk probe (offload_probe, DEV_TOOLS): game thread reads of flush segment results on an instruction clock.

#include <cstdint>

namespace win32 { namespace d3d9 { namespace probe {

enum Read : uint8_t
{
    ReadRender, ReadStage, ReadSampler, ReadTransform, ReadViewport, ReadTexture, ReadStream, ReadIndices,
    ReadMaterial, ReadLight, ReadLightEnable, ReadDeclaration, ReadFvf, ReadTarget, ReadDepth, ReadEffectValue,
    ReadEffectTexture, ReadCount
};
enum Scope : uint8_t { ScopeGuest, ScopeBegin, ScopeEnd, ScopeCount };
enum Counter : uint8_t
{
    LayoutMissParent, LayoutMissClone, SyncStorageParent, SyncStorageClone, PassInfoParent, PassInfoClone,
    ReleasePending, ReleaseOther, DestroyPending, DestroyOther, SyncWords, SyncBlocks, MergedFloats, MergedChanged,
    MergedTextures, MergedTexturesChanged, ParentStoreOnFlushFloat, ParentStoreSame, ParentDraws, ParentDrawsFlushFloats,
    ParentDrawsFlushTexture, ParentTextureSetOnFlush, ArenaReserved, ArenaUsed, ArenaSegments, ArenaShrinks,
    PoolFrees, SegmentDraws, SkinnedCalls, SkinnedPoolAlias, ParentStoresAfter, ParentStoreWordsAfter,
    ParentStoresSameAfter, ParentStoreWordsSameAfter, CounterCount
};

#ifdef DEV_TOOLS
extern bool g_on, g_inSegment, g_pending;
extern uint8_t g_scope;

void configure();
// Device state read: FixedState word index or binding slot.
void deviceRead(Read kind, bool clone, uint32_t word, uint32_t esp);
// The game thread set fixed state words.
void parentSet(uint32_t word, uint32_t count);
// The flush runs on its context between the two calls.
void segmentRun(bool begin);
// Parent and clone state at the segment's end.
void segmentClosed(const uint32_t* parent, const uint32_t* clone, uint32_t words, const void* const* parentSlots,
                   const void* const* cloneSlots, uint32_t slots);
// Merged float or texture; changed: the parent's value differed.
void merged(uint32_t block, uint32_t offset, bool changed, uint32_t param);
void mergedTexture(uint32_t block, uint32_t param, bool changed);
// Game thread effect store, read, texture set and draw.
void parentStore(uint32_t block, uint32_t offset, uint32_t count, bool same);
void parentTexture(uint32_t block, uint32_t param);
void effectRead(uint32_t block, uint32_t offset, uint32_t count, bool texture);
void parentDraw(uint32_t kind, uint32_t pass, uint32_t own, uint32_t shared, const uint8_t* textureParams,
                uint32_t samplers);
void count(Counter c, uint64_t n = 1);
void frameEnd();
// Parameter names for the report.
extern const char* (*g_paramName)(uint32_t param);
extern const char* (*g_effectName)(uint32_t kind);

inline bool on() { return g_on; }
#else
inline bool on() { return false; }
#endif

}}}

#endif /* !WINAPI_D3D9_PROBE_H_ */
