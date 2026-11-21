#ifndef WINAPI_D3D9_INTERNAL_H_
#define WINAPI_D3D9_INTERNAL_H_

// Direct3D 9 layer: COM objects in guest memory, Direct3D types, the device state machine that records frames
// (platform/render.h) and the binding the effect layer (d3dx9.cpp) gives the device.
//
// A guest COM object is 8 bytes of guest memory: its vtable and a handle. The handle indexes the host object
// table; vtable slots hold their own guest address, under which the method is registered.

#include <lib/winapp.h>
#include <lib/memmap.h>
#include <winapi/types.h>
#include <winapi/d3d9_native.h>
#include <winapi/wrapper.h>
#include <platform/log.h>
#include <platform/render.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace win32 { namespace d3d9
{

namespace render = pf::render;
namespace texture = pf::texture;

// Bitwise equality of count dwords at a and b.
inline bool sameWords(const void* a, const void* b, uint32_t count)
{
    const uint8_t* p = static_cast<const uint8_t*>(a);
    const uint8_t* q = static_cast<const uint8_t*>(b);
    const auto same = [&](uint32_t offset)
    {
        uint32_t x, y;
        memcpy(&x, p + offset, 4);
        memcpy(&y, q + offset, 4);
        return x == y;
    };
    for (; count >= 4; count -= 4, p += 16, q += 16)
        if (!same(0) || !same(4) || !same(8) || !same(12))
            return false;
    for (; count; --count, p += 4, q += 4)
        if (!same(0))
            return false;
    return true;
}

// ---- Constants and types -------------------------------------------------------------------------------------------

enum : HRESULT
{
    D3D_OK                  = 0,
    D3DERR_NOTFOUND         = 0x88760866,
    D3DERR_NOTAVAILABLE     = 0x8876086A,
    D3DERR_INVALIDCALL      = 0x8876086C,
    D3DERR_OUTOFVIDEOMEMORY = 0x8876017C,
    E_NOINTERFACE           = 0x80004002,
    E_FAIL                  = 0x80004005
};

enum : DWORD
{
    D3DFMT_UNKNOWN  = 0,
    D3DFMT_A8R8G8B8 = 21,
    D3DFMT_X8R8G8B8 = 22,
    D3DFMT_D24S8    = 75,
    D3DFMT_D24X8    = 77,
    D3DFMT_D16      = 80,
    D3DFMT_INDEX16  = 101,
    D3DFMT_INDEX32  = 102,
    D3DFMT_DXT1     = 0x31545844,
    D3DFMT_DXT3     = 0x33545844,
    D3DFMT_DXT5     = 0x35545844
};

enum : DWORD
{
    D3DUSAGE_RENDERTARGET   = 0x1,
    D3DUSAGE_DEPTHSTENCIL   = 0x2,
    D3DUSAGE_DYNAMIC        = 0x200,

    D3DLOCK_READONLY        = 0x10,
    D3DLOCK_DISCARD         = 0x2000,
    D3DLOCK_NOOVERWRITE     = 0x1000,

    D3DRTYPE_SURFACE        = 1,
    D3DRTYPE_TEXTURE        = 3,
    D3DRTYPE_CUBETEXTURE    = 5,
    D3DRTYPE_VERTEXBUFFER   = 6,
    D3DRTYPE_INDEXBUFFER    = 7
};

struct D3DMATRIX
{
    float m[16];        // rows
};

struct D3DVIEWPORT9
{
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

struct D3DPRESENT_PARAMETERS
{
    UINT    BackBufferWidth;
    UINT    BackBufferHeight;
    DWORD   BackBufferFormat;
    UINT    BackBufferCount;
    DWORD   MultiSampleType;
    DWORD   MultiSampleQuality;
    DWORD   SwapEffect;
    HWND    hDeviceWindow;
    BOOL    Windowed;
    BOOL    EnableAutoDepthStencil;
    DWORD   AutoDepthStencilFormat;
    DWORD   Flags;
    UINT    FullScreen_RefreshRateInHz;
    UINT    PresentationInterval;
};

struct D3DDISPLAYMODE
{
    UINT    Width, Height, RefreshRate;
    DWORD   Format;
};

struct D3DLOCKED_RECT
{
    LONG    Pitch;
    DWORD   pBits;      // guest address
};

struct D3DSURFACE_DESC
{
    DWORD   Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
    UINT    Width, Height;
};

struct D3DVERTEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
    DWORD   FVF;
};

struct D3DINDEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
};

struct D3DVERTEXELEMENT9
{
    WORD    Stream;
    WORD    Offset;
    BYTE    Type;
    BYTE    Method;
    BYTE    Usage;
    BYTE    UsageIndex;
};

struct D3DRECT
{
    LONG x1, y1, x2, y2;
};

struct D3DADAPTER_IDENTIFIER9
{
    char    Driver[512];
    char    Description[512];
    char    DeviceName[32];
    DWORD   DriverVersionLowPart;
    DWORD   DriverVersionHighPart;
    DWORD   VendorId;
    DWORD   DeviceId;
    DWORD   SubSysId;
    DWORD   Revision;
    GUID    DeviceIdentifier;
    DWORD   WHQLLevel;
};

struct D3DDEVICE_CREATION_PARAMETERS
{
    UINT    AdapterOrdinal;
    DWORD   DeviceType;
    HWND    hFocusWindow;
    DWORD   BehaviorFlags;
};

// D3DCAPS9 as 76 DWORDs; the indices of the fields this layer sets.
enum CapsField
{
    CapsDeviceType = 0, CapsAdapterOrdinal, CapsCaps, CapsCaps2, CapsCaps3, CapsPresentationIntervals,
    CapsCursorCaps, CapsDevCaps, CapsPrimitiveMiscCaps, CapsRasterCaps, CapsZCmpCaps, CapsSrcBlendCaps,
    CapsDestBlendCaps, CapsAlphaCmpCaps, CapsShadeCaps, CapsTextureCaps, CapsTextureFilterCaps,
    CapsCubeTextureFilterCaps, CapsVolumeTextureFilterCaps, CapsTextureAddressCaps, CapsVolumeTextureAddressCaps,
    CapsLineCaps, CapsMaxTextureWidth, CapsMaxTextureHeight, CapsMaxVolumeExtent, CapsMaxTextureRepeat,
    CapsMaxTextureAspectRatio, CapsMaxAnisotropy, CapsMaxVertexW, CapsGuardBandLeft, CapsGuardBandTop,
    CapsGuardBandRight, CapsGuardBandBottom, CapsExtentsAdjust, CapsStencilCaps, CapsFVFCaps, CapsTextureOpCaps,
    CapsMaxTextureBlendStages, CapsMaxSimultaneousTextures, CapsVertexProcessingCaps, CapsMaxActiveLights,
    CapsMaxUserClipPlanes, CapsMaxVertexBlendMatrices, CapsMaxVertexBlendMatrixIndex, CapsMaxPointSize,
    CapsMaxPrimitiveCount, CapsMaxVertexIndex, CapsMaxStreams, CapsMaxStreamStride, CapsVertexShaderVersion,
    CapsMaxVertexShaderConst, CapsPixelShaderVersion, CapsPixelShader1xMaxValue, CapsDevCaps2,
    CapsMaxNpatchTessellationLevel, CapsReserved5, CapsMasterAdapterOrdinal, CapsAdapterOrdinalInGroup,
    CapsNumberOfAdaptersInGroup, CapsDeclTypes, CapsNumSimultaneousRTs, CapsStretchRectFilterCaps,
    CapsCount = 76
};

struct D3DCAPS9
{
    DWORD field[CapsCount];
};

// The guest's layouts on every host.
static_assert(sizeof(D3DMATRIX) == 64 && sizeof(D3DVIEWPORT9) == 24 && sizeof(D3DPRESENT_PARAMETERS) == 56 &&
              sizeof(D3DDISPLAYMODE) == 16 && sizeof(D3DLOCKED_RECT) == 8 && sizeof(D3DSURFACE_DESC) == 32 &&
              sizeof(D3DVERTEXBUFFER_DESC) == 24 && sizeof(D3DINDEXBUFFER_DESC) == 20 &&
              sizeof(D3DVERTEXELEMENT9) == 8 && sizeof(D3DRECT) == 16 && sizeof(D3DADAPTER_IDENTIFIER9) == 1100 &&
              sizeof(D3DDEVICE_CREATION_PARAMETERS) == 16 && sizeof(D3DCAPS9) == 304, "Direct3D 9 layouts");

// ---- Objects -------------------------------------------------------------------------------------------------------

enum Kind
{
    KindDirect3D,
    KindDevice,
    KindTexture,
    KindCubeTexture,
    KindSurface,
    KindVertexBuffer,
    KindIndexBuffer,
    KindVertexDeclaration,
    KindEffect,
    KindEffectPool,
    KindCount
};

class Object
{
public:
    explicit Object(Kind kind) : m_kind(kind), m_refs(1), m_guest(0) {}
    virtual ~Object();

    Kind kind() const               { return m_kind; }
    x86::reg32 guest() const        { return m_guest; }
    // Deletes the object (and frees its guest block) on the last reference. Texture surfaces count their
    // container's references, as in Direct3D.
    virtual ULONG addRef()          { return ++m_refs; }
    virtual ULONG release();
    // A reference of a binding that deferred calls change (device texture stages, vertex declaration, stream and
    // indices; effect textures), counted apart as well.
    virtual void bind()             { ++m_bindings; addRef(); }
    virtual ULONG unbind()          { --m_bindings; return release(); }
    // bind() and unbind() without virtual calls, for kinds that override no counting method (final classes).
    void bindPlain()                { ++m_bindings; ++m_refs; }
    ULONG unbindPlain()
    {
        --m_bindings;
        if (m_refs > 1 && (m_refs - 1 != m_bindings || m_boundOnly))
            return --m_refs;
        return Object::release();
    }
    // The object whose counts addRef and release change.
    virtual Object* counter()       { return this; }
    // Device-held reference on implicit surfaces; guest Release cannot drop it.
    void pin()                      { ++m_pins; }
    void unpin()                    { --m_pins; }

    HRESULT QueryInterface(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* object);
    ULONG AddRef(WinApplication* app, x86::CPU& cpu);
    ULONG Release(WinApplication* app, x86::CPU& cpu);

private:
    friend x86::reg32 publish(WinApplication* app, Object* object);

    Kind        m_kind;
    uint32_t    m_refs;
    x86::reg32  m_guest;
    uint32_t    m_bindings = 0;
    uint32_t    m_pins = 0;
    bool        m_boundOnly = false;    // counted in bridge::countBoundOnly
};

// Gives a host object its guest COM object (the vtable of its kind) and returns the guest address.
x86::reg32 publish(WinApplication* app, Object* object);
// The host object of a guest COM pointer; null for 0 or an unknown pointer (logged).
Object* lookup(x86::reg32 guest);
// Builds the vtables and registers the methods, once.
void initInterfaces(WinApplication* app);

class Device;

namespace bridge
{

// True on the thread that runs deferred calls (d3d9_native.h).
bool onQueueThread();
// Queue thread: the object is deleted by the next drain.
void deleteLater(Object* object);
// While segments are pending, objects that reach zero references wait to be deleted at the last join.
extern bool g_deferDeletes;
// Segments opened and not yet closed; effect stores are not skipped while any is.
extern uint32_t g_pendingSegments;
extern void (*g_frameHook)(uint32_t frame);
void deleteAtJoin(Object* object);
// One segment less pending; the last one deletes what waited. Game thread.
void joinDeletes();
// Objects that only bindings hold: +1 when one becomes so, -1 when it is deleted. While there are any, calls that
// change bindings are not deferred. Game thread.
void countBoundOnly(int delta);
// A device is deleted.
void deviceGone(const Device* device);

// The device of a guest COM pointer, cached in g_hot until objects change.
Device* deviceLookup(reg32 guest);
inline Device* deviceObject(reg32 guest)
{
    if (__builtin_expect(guest == g_hot.deviceGuest, 1))
        return static_cast<Device*>(g_hot.device);
    return deviceLookup(guest);
}

// Segment clone when cpu is a segment's, else the device.
inline Device* deviceOf(const x86::CPU& cpu, reg32 guest)
{
    if (__builtin_expect(cpu.og != nullptr, 0))
        return static_cast<Device*>(static_cast<const SegmentRef*>(cpu.og)->device);
    return deviceObject(guest);
}

// The bridge methods whose quiet case runs beside the method (d3d9_device.cpp, d3dx9.cpp): every case, with deferral
// and capture (d3d9_native.cpp).
HRESULT setRenderStateSlow(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD state, DWORD value);
HRESULT setVertexDeclarationSlow(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 declaration);
HRESULT setStreamSourceSlow(WinApplication* app, x86::CPU& cpu, reg32 device, UINT stream, reg32 buffer, UINT offset,
                            UINT stride);
HRESULT setIndicesSlow(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 buffer);
HRESULT drawIndexedPrimitiveSlow(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD type, LONG baseVertex,
                                 UINT minIndex, UINT vertices, UINT startIndex, UINT count);
HRESULT drawPrimitiveUPSlow(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD type, UINT count, const void* data,
                            UINT stride);
HRESULT effectSetFloatSlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value);
HRESULT effectSetVectorSlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector);
HRESULT effectSetIntArraySlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle,
                              const x86::sreg32* values, UINT count);
HRESULT effectSetVectorArraySlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                                 UINT count);
HRESULT effectSetMatrixSlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix);
HRESULT effectSetTextureSlow(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, reg32 texture);
HRESULT effectPassSlow(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass);

}

// Counts object publications and deletions.
extern uint32_t g_objectGeneration;
// The device whose frame stream bridge::g_hot.stream mirrors (the first one created).
extern Device* g_streamDevice;
// An object was published or deleted: bumps the generation and clears the bridge's lookup caches.
void objectsChanged();

// A guest method that drains the deferred calls (d3d9_native.h) before it runs; cpu.ip is its slot address.
template< MethodPtr M >
void drained(WinApplication* app, x86::CPU& cpu)
{
    bridge::drain(cpu.ip);
    M(app, cpu);
    bridge::afterDirect();
}

// drained, noting bindings only after a drain (effect methods).
template< MethodPtr M >
void drainedNoBindings(WinApplication* app, x86::CPU& cpu)
{
    const bool pending = bridge::g_hot.pending;
    bridge::drain(cpu.ip);
    M(app, cpu);
    if (__builtin_expect(pending, 0))
        bridge::afterDirect();
}

// A vtable slot: the method, or null for one that logs its name and guest address and traps.
struct MethodEntry
{
    const char* name;
    MethodPtr   method;
};
// Builds the vtable of a kind; slots in interface order.
void buildInterface(WinApplication* app, Kind kind, const char* interfaceName, const MethodEntry* methods,
                    uint32_t count);
// The effect interfaces (d3dx9.cpp).
void initEffectInterfaces(WinApplication* app);

// Guest entries of the methods native code calls directly (d3d9_native.cpp, d3d9_native.h).
namespace guest
{

void SetTransform(WinApplication* app, x86::CPU& cpu);
void SetMaterial(WinApplication* app, x86::CPU& cpu);
void SetLight(WinApplication* app, x86::CPU& cpu);
void LightEnable(WinApplication* app, x86::CPU& cpu);
void SetRenderState(WinApplication* app, x86::CPU& cpu);
void SetTextureStageState(WinApplication* app, x86::CPU& cpu);
void SetSamplerState(WinApplication* app, x86::CPU& cpu);
void Present(WinApplication* app, x86::CPU& cpu);
void SetViewport(WinApplication* app, x86::CPU& cpu);
void SetFVF(WinApplication* app, x86::CPU& cpu);
void Clear(WinApplication* app, x86::CPU& cpu);
void BeginScene(WinApplication* app, x86::CPU& cpu);
void EndScene(WinApplication* app, x86::CPU& cpu);
void SetRenderTarget(WinApplication* app, x86::CPU& cpu);
void SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu);
void SetTexture(WinApplication* app, x86::CPU& cpu);
void SetVertexDeclaration(WinApplication* app, x86::CPU& cpu);
void SetStreamSource(WinApplication* app, x86::CPU& cpu);
void SetIndices(WinApplication* app, x86::CPU& cpu);
void DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu);
void DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu);
void EffectSetInt(WinApplication* app, x86::CPU& cpu);
void EffectSetIntArray(WinApplication* app, x86::CPU& cpu);
void EffectSetFloat(WinApplication* app, x86::CPU& cpu);
void EffectSetVector(WinApplication* app, x86::CPU& cpu);
void EffectSetVectorArray(WinApplication* app, x86::CPU& cpu);
void EffectSetMatrix(WinApplication* app, x86::CPU& cpu);
void EffectSetMatrixArray(WinApplication* app, x86::CPU& cpu);
void EffectSetTexture(WinApplication* app, x86::CPU& cpu);
void EffectBegin(WinApplication* app, x86::CPU& cpu);
void EffectPass(WinApplication* app, x86::CPU& cpu);
void EffectEnd(WinApplication* app, x86::CPU& cpu);

}

#define D3D_GUEST(fn) { #fn, &win32::d3d9::guest::fn }
#define D3D_GUEST_NAMED(name, fn) { name, &win32::d3d9::guest::fn }

#define D3D_METHOD(cls, fn) { #fn, &win32::d3d9::drained<&win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall> }
#define D3D_NAMED(name, cls, fn) { name, &win32::d3d9::drained<&win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall> }
// A drained method that changes no device binding (drainedNoBindings).
#define D3D_UNBOUND(cls, fn) { #fn, &win32::d3d9::drainedNoBindings<&win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall> }
// A method that reads only what its object was created with: no drain.
#define D3D_FIXED(cls, fn) { #fn, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_FIXED_NAMED(name, cls, fn) { name, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_TRAP(fn) { #fn, nullptr }

// Logs a message once per call site.
#define D3D_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; pf::logInfo(__VA_ARGS__); } } while (0)

class Device;
class Texture;
struct KeptLevel;
class Surface;
class Buffer;
class VertexDeclaration;

class Direct3D : public Object
{
public:
    Direct3D() : Object(KindDirect3D) {}

    HRESULT RegisterSoftwareDevice(WinApplication* app, x86::CPU& cpu, x86::reg32 init);
    UINT GetAdapterCount(WinApplication* app, x86::CPU& cpu);
    HRESULT GetAdapterIdentifier(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD flags,
                                 D3DADAPTER_IDENTIFIER9* identifier);
    UINT GetAdapterModeCount(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format);
    HRESULT EnumAdapterModes(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format, UINT mode,
                             D3DDISPLAYMODE* out);
    HRESULT GetAdapterDisplayMode(WinApplication* app, x86::CPU& cpu, UINT adapter, D3DDISPLAYMODE* out);
    HRESULT CheckDeviceType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD displayFormat,
                            DWORD backFormat, BOOL windowed);
    HRESULT CheckDeviceFormat(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                              DWORD usage, DWORD resourceType, DWORD format);
    HRESULT CheckDeviceMultiSampleType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD format,
                                       BOOL windowed, DWORD multiSample, DWORD* qualityLevels);
    HRESULT CheckDepthStencilMatch(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                                   DWORD targetFormat, DWORD depthFormat);
    HRESULT CheckDeviceFormatConversion(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                        DWORD sourceFormat, DWORD targetFormat);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, D3DCAPS9* caps);
    HANDLE GetAdapterMonitor(WinApplication* app, x86::CPU& cpu, UINT adapter);
    HRESULT CreateDevice(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, HWND window, DWORD flags,
                         D3DPRESENT_PARAMETERS* params, x86::reg32* device);

    static void fillCaps(D3DCAPS9* caps);
};

// Textures, cube textures and their surfaces.
class Texture : public Object
{
public:
    Texture(Device* device, const render::TextureDesc& desc, DWORD format, DWORD usage, DWORD pool);
    ~Texture() override;

    render::TextureStorage* storage() const    { return m_storage; }
    const render::TextureDesc& desc() const    { return m_desc; }
    DWORD format() const                        { return m_format; }
    Device* device() const                      { return m_device; }
    bool valid() const                          { return m_storage != nullptr; }
    void setOpaque()                            { m_opaque = true; }
    void fillDesc(UINT level, D3DSURFACE_DESC* desc) const;
    // The surface of a level (and face), kept while the texture lives; one reference added for the caller.
    Surface* surface(WinApplication* app, uint32_t face, uint32_t level);
    // Game thread, once per frame: drops kept linear copies over the byte cap whose uploads are finished.
    static void trimKept();

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    DWORD SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod);
    DWORD GetLOD(WinApplication* app, x86::CPU& cpu);
    DWORD GetLevelCount(WinApplication* app, x86::CPU& cpu);
    HRESULT GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DSURFACE_DESC* desc);
    HRESULT GetSurfaceLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* surface);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                     DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu, UINT level);
    HRESULT AddDirtyRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT GetCubeMapSurface(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, x86::reg32* surface);
    HRESULT LockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, D3DLOCKED_RECT* locked,
                         const RECT* rect, DWORD flags);
    HRESULT UnlockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level);
    HRESULT AddDirtyRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, const RECT* rect);

private:
    HRESULT lock(uint32_t face, uint32_t level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT unlock(uint32_t face, uint32_t level);
    // Read-only copy of a render target as drawn so far.
    HRESULT lockTarget(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect);
    // The level's kept linear copy, taken out of the cache; null when there is none.
    MemMap* takeKept(uint32_t index, uint32_t* ticket);
    void keep(uint32_t index, MemMap* memory, uint32_t ticket);

    friend struct KeptLevels;

    Device*                 m_device;
    render::TextureDesc     m_desc;
    DWORD                   m_format, m_usage, m_pool;
    render::TextureStorage* m_storage;
    std::vector<MemMap*>    m_locks;        // per face * levels + level: the guest buffer of a lock
    std::vector<uint32_t>   m_tickets;      // the upload reading a locked buffer (0: none)
    std::vector<DWORD>      m_lockFlags;
    std::vector<bool>       m_written;      // the level has been written since creation
    std::vector<uint8_t>    m_lockCounts;   // locks so far, saturating at 2
    std::vector<KeptLevel*> m_kept;         // the linear copy kept after the last unlock (KeptLevels lock)
    std::vector<Surface*>   m_surfaces;
    bool                    m_opaque = false;   // copied from the back buffer, which has no alpha
};

class Surface : public Object
{
public:
    enum Role
    {
        RoleLevel,          // a level (face) of a texture
        RoleBackBuffer,
        RoleDepth
    };

    Surface(Device* device, Role role, uint32_t width, uint32_t height, DWORD format);
    ~Surface() override;

    Role role() const                       { return m_role; }
    uint32_t width() const                  { return m_width; }
    uint32_t height() const                 { return m_height; }
    Texture* container() const              { return m_container; }
    uint32_t face() const                   { return m_face; }
    uint32_t level() const                  { return m_level; }
    render::DepthStorage* depth() const     { return m_depth; }
    void setContainer(Texture* texture, uint32_t face, uint32_t level);
    void setDepth(render::DepthStorage* depth) { m_depth = depth; }
    ULONG addRef() override;
    ULONG release() override;
    void bind() override;
    ULONG unbind() override;
    Object* counter() override;

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container);
    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, D3DSURFACE_DESC* desc);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu);

private:
    Device*                 m_device;
    Role                    m_role;
    uint32_t                m_width, m_height;
    DWORD                   m_format;
    Texture*                m_container = nullptr;
    uint32_t                m_face = 0, m_level = 0;
    render::DepthStorage*   m_depth = nullptr;      // owned for RoleDepth
};

// Vertex and index buffers: the guest writes a shadow copy in guest memory; unlocks copy into the GPU buffer,
// a new one when the current is still read by frames in flight (unless NOOVERWRITE).
class Buffer final : public Object
{
public:
    Buffer(Device* device, Kind kind, UINT size, DWORD usage, DWORD pool, DWORD format);
    ~Buffer() override;

    render::BufferStorage* storage() const  { return m_storage; }
    bool valid() const                      { return m_storage != nullptr; }
    UINT size() const                       { return m_size; }
    DWORD format() const                    { return m_format; }
    // Guest address of the memory Lock hands out (offset 0); it keeps the bytes written until the next write.
    x86::reg32 shadowAddress() const;
    // Game thread: called at each last Unlock with the buffer's guest COM pointer and the locked range of its
    // memory, before the bytes go to the GPU copy; null stops it.
    typedef void (*UnlockObserver)(x86::reg32 buffer, x86::reg32 data, UINT size);
    static void setUnlockObserver(UnlockObserver observer);

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT Lock(WinApplication* app, x86::CPU& cpu, UINT offset, UINT size, x86::reg32* data, DWORD flags);
    HRESULT Unlock(WinApplication* app, x86::CPU& cpu);
    HRESULT GetVertexDesc(WinApplication* app, x86::CPU& cpu, D3DVERTEXBUFFER_DESC* desc);
    HRESULT GetIndexDesc(WinApplication* app, x86::CPU& cpu, D3DINDEXBUFFER_DESC* desc);
    // Game thread: brings a stale GPU copy up to date before a segment draws from it.
    void syncIfStale()                      { if (m_stale) syncStorage(); }
    // Guest pointers of up to max stale buffers; returns their number.
    static uint32_t staleGuests(x86::reg32* out, uint32_t max);
    // A draw's bytes start..end: the frame copy of the last DISCARD lock, else the GPU copy.
    const uint8_t* drawData(uint32_t frameIndex, uint64_t start, uint64_t end, render::BufferStorage** storage);

private:
    // Gives the GPU copy every byte of the shadow.
    void syncStorage();
    void setStale(bool stale);

    Device*                 m_device;
    UINT                    m_size;
    DWORD                   m_usage, m_pool, m_format;
    MemMap*                 m_shadow;
    render::BufferStorage*  m_storage;
    UINT                    m_lockOffset = 0, m_lockSize = 0;
    DWORD                   m_lockFlags = 0;
    int                     m_locks = 0;
    uintptr_t               m_frameData = 0;        // frame copy of the last DISCARD lock, as buffer offset 0
    uint32_t                m_frameIndex = 0;
    UINT                    m_frameStart = 0, m_frameEnd = 0;
    bool                    m_stale = false;        // the GPU copy lacks the frame copy's bytes
};

class VertexDeclaration final : public Object
{
public:
    VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t count, DWORD fvf);

    DWORD fvf() const                           { return m_fvf; }
    // The layout of these elements at a stream stride (registered once per stride).
    uint8_t layout(uint32_t stride);
    // The same with a cache of its own, for the segments (one thread at a time) beside the game thread's.
    uint8_t layoutAt(uint32_t stride);
    // Components of the TEXCOORD0 input (0: none).
    uint32_t texComponents() const              { return m_texComponents; }
    bool positionT() const                      { return m_layout.positionT; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT GetDeclaration(WinApplication* app, x86::CPU& cpu, D3DVERTEXELEMENT9* elements, UINT* count);

    // Layout of an FVF code; stride from the FVF.
    static render::VertexLayout fvfLayout(DWORD fvf, uint32_t* texComponents);

private:
    Device*                         m_device;
    std::vector<D3DVERTEXELEMENT9>  m_elements;
    DWORD                           m_fvf;
    render::VertexLayout            m_layout;   // stride 0
    uint32_t                        m_texComponents = 0;
    uint32_t                        m_stride = 0;
    uint8_t                         m_layoutId = 0;
    uint32_t                        m_segmentStride = 0;
    uint8_t                         m_segmentLayoutId = 0;
};

// ---- Recording: the game thread records state changes and draws; the render thread resolves them -----------------

// Commands of a frame's stream: a header word (Command in bits 0-7, arguments above) and payload words.
enum Command : uint8_t
{
    CmdRenderState,         // header: state << 8; value
    CmdStageState,          // header: stage << 8 | type << 16; value
    CmdSamplerState,        // header: sampler << 8 | type << 16; value
    CmdTransform,           // header: transform << 8 (Transform); 16 floats
    CmdViewport,            // D3DVIEWPORT9
    CmdFixedState,          // FixedState, whole
    CmdValues,              // header: block << 8; offset | count << 16; count floats
    CmdDraw,                // DrawRecord
    CmdProduct,             // header: block << 8; offset | count << 16; FPSCR; count matrices, their product
    CmdDrawCall,            // index of a draw the recorder resolved whole
    CmdSplice,              // SpliceRecord: runs a segment's commands
    CmdSkipIf               // header: flag << 8; words to skip while the flag is set
};

// The VFP status and control register of the calling thread: FPCR on AArch64 (the same control bits), 0 and no
// effect elsewhere.
inline uint32_t readFpscr()
{
#if defined(__arm__)
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
#elif defined(__aarch64__)
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return uint32_t(value);
#else
    return 0;
#endif
}

inline void writeFpscr(uint32_t value)
{
#if defined(__arm__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(uint64_t(value)) : "memory");
#else
    static_cast<void>(value);
#endif
}

// out = m[0] * m[1] (* m[2]) for count 2 or 3, multiplied left to right as D3DXMatrixMultiply does, with the
// thread's FPSCR set to fpscr.
void matrixProduct(const float* matrices, uint32_t count, uint32_t fpscr, float* out);

enum Transform : uint8_t
{
    TransformWorld, TransformView, TransformProjection, TransformTexture0, TransformCount
};

// Growing word buffer, one per frame in flight.
struct ValuesShadow;

struct Stream
{
    uint32_t*       data = nullptr;
    uint32_t        size = 0, capacity = 0;
    uint32_t        minimum = 64 * 1024;    // words of its first allocation
    ValuesShadow*   shadow = nullptr;       // a segment's effect value shadow; null in the frame stream

    uint32_t* append(uint32_t words)
    {
        if (size + words > capacity)
            grow(words);
        uint32_t* p = data + size;
        size += words;
        return p;
    }
    void grow(uint32_t words);
};

// The stream of a frame index, emptied; game thread.
Stream& frameStream(uint32_t frameIndex);
// Render thread: completes the draws of a frame from its stream (Frame::resolve).
void resolveFrame(render::Frame& frame);
// Tests: the resolver forgets the state it mirrors, and which frames it resolved.
void resetResolver();

// Device state the draws read, mirrored on the render thread.
struct FixedState
{
    DWORD           renderStates[256];
    DWORD           stageStates[8][33];
    DWORD           samplerStates[16][14];
    D3DMATRIX       transforms[TransformCount];
    D3DVIEWPORT9    viewport;
};

// The frame stream aligns its records to 4 bytes only, which host pointers exceed on a 64-bit host.
#if UINTPTR_MAX > 0xffffffffu
#define D3D_STREAM_RECORD __attribute__((packed, aligned(4)))
#else
#define D3D_STREAM_RECORD
#endif

struct SegmentBuf;

// A stream's commands (one whole flush) run by the resolver at a CmdSplice.
struct Segment
{
    const uint32_t* words = nullptr;
    uint32_t        size = 0;
    uint32_t        drawCount = 0;      // draws it recorded into the slots reserved for it
    std::atomic<uint32_t> ready{1};     // 0: another thread still records it
    void          (*await)(void* job) = nullptr;    // returns when it is ready; may run the job
    void*           job = nullptr;
};

struct D3D_STREAM_RECORD SpliceRecord
{
    const Segment*  segment;
    uint32_t        flags;      // set bits make CmdSkipIf commands of the segment skip their words
    uint32_t        scene;      // the scene of the reserved draw slots
    uint32_t        reserved;   // draw slots kept for the segment; unused ones are closed up by the resolver
};

// Recorder state of one command stream: where it appends and what the stream's reader has after the commands so far.
// A context with unknown words records the first set of each of them.
extern uint64_t g_extraStateWords[2];   // words recorded only as unknown after a boundary: parent, segment
#ifdef DEV_TOOLS
namespace probe { extern bool g_on; void parentSet(uint32_t word, uint32_t count); }
#define D3D_PROBE_SET(at, n) do { if (probe::g_on && !segment) probe::parentSet(uint32_t(reinterpret_cast<const uint32_t*>(at) - reinterpret_cast<const uint32_t*>(&fixed)), n); } while (0)
#else
#define D3D_PROBE_SET(at, n) do {} while (0)
#endif

struct RecordContext
{
    Stream*     stream = nullptr;
    FixedState  fixed = {};
    bool        partial = false;        // some words of fixed are unknown
    SegmentBuf* segment = nullptr;      // the open segment whose stream this is, if any
    std::vector<uint64_t>* undo = nullptr;  // while a segment is open: fixed words (index << 32) and their old values

    void unknownAll()
    {
        partial = true;
        memset(m_known, 0, sizeof(m_known));
    }
    // Whether setting the word needs a command, and the set itself.
    bool renderNeeds(uint32_t state, DWORD value) const
    {
        D3D_PROBE_SET(&fixed.renderStates[state], 1);
        return need(fixed.renderStates[state] != value, RenderBase + state);
    }
    void renderSet(uint32_t state, DWORD value)
    {
        if (undo)
            log(&fixed.renderStates[state], 1);
        fixed.renderStates[state] = value;
        if (partial)
            mark(RenderBase + state);
    }
    bool stageNeeds(uint32_t stage, uint32_t type, DWORD value) const
    {
        D3D_PROBE_SET(&fixed.stageStates[stage][type], 1);
        return need(fixed.stageStates[stage][type] != value, StageBase + stage * 33 + type);
    }
    void stageSet(uint32_t stage, uint32_t type, DWORD value)
    {
        if (undo)
            log(&fixed.stageStates[stage][type], 1);
        fixed.stageStates[stage][type] = value;
        if (partial)
            mark(StageBase + stage * 33 + type);
    }
    bool samplerNeeds(uint32_t sampler, uint32_t type, DWORD value) const
    {
        D3D_PROBE_SET(&fixed.samplerStates[sampler][type], 1);
        return need(fixed.samplerStates[sampler][type] != value, SamplerBase + sampler * 14 + type);
    }
    void samplerSet(uint32_t sampler, uint32_t type, DWORD value)
    {
        if (undo)
            log(&fixed.samplerStates[sampler][type], 1);
        fixed.samplerStates[sampler][type] = value;
        if (partial)
            mark(SamplerBase + sampler * 14 + type);
    }
    bool transformNeeds(uint32_t transform, const D3DMATRIX& value) const
    {
        D3D_PROBE_SET(&fixed.transforms[transform], 16);
        return need(memcmp(&fixed.transforms[transform], &value, sizeof(D3DMATRIX)) != 0, TransformBase + transform);
    }
    void transformSet(uint32_t transform, const D3DMATRIX& value)
    {
        if (undo && !(m_touched >> transform & 1))
        {
            m_touched |= 1u << transform;
            log(&fixed.transforms[transform], sizeof(D3DMATRIX) / 4);
        }
        fixed.transforms[transform] = value;
        if (partial)
            mark(TransformBase + transform);
    }
    bool viewportNeeds(const D3DVIEWPORT9& value) const
    {
        D3D_PROBE_SET(&fixed.viewport, 6);
        return need(memcmp(&fixed.viewport, &value, sizeof(value)) != 0, ViewportBit);
    }
    void viewportSet(const D3DVIEWPORT9& value)
    {
        if (undo && !(m_touched >> 4 & 1))
        {
            m_touched |= 1u << 4;
            log(&fixed.viewport, sizeof(D3DVIEWPORT9) / 4);
        }
        fixed.viewport = value;
        if (partial)
            mark(ViewportBit);
    }

private:
    enum : uint32_t
    {
        RenderBase = 0, StageBase = 256, SamplerBase = 256 + 8 * 33, TransformBase = SamplerBase + 16 * 14,
        ViewportBit = TransformBase + 4, Bits = ViewportBit + 1
    };

public:
    // The words and knowledge to put back when the segment ends: the segment records on the parent's state.
    struct Saved
    {
        std::vector<uint64_t>   log;
        uint64_t                known[(Bits + 63) / 64];
        bool                    partial;
    };
    void open(Saved& s)
    {
        s.log.clear();
        s.partial = partial;
        memcpy(s.known, m_known, sizeof(m_known));
        m_touched = 0;
        undo = &s.log;
    }
    // Starts from the parent's words; nothing to put back.
    void openClone(const RecordContext& p)
    {
        memcpy(&fixed, &p.fixed, sizeof(fixed));
        partial = p.partial;
        memcpy(m_known, p.m_known, sizeof(m_known));
        m_touched = 0;
        undo = nullptr;
    }
    void close(Saved& s)
    {
        uint32_t* words = reinterpret_cast<uint32_t*>(&fixed);
        for (size_t i = s.log.size(); i > 0; --i)
            words[s.log[i - 1] >> 32] = uint32_t(s.log[i - 1]);
        undo = nullptr;
        partial = s.partial;
        memcpy(m_known, s.known, sizeof(m_known));
    }

private:
    void log(const void* at, uint32_t words)
    {
        const uint32_t first = uint32_t(static_cast<const uint32_t*>(at) - reinterpret_cast<const uint32_t*>(&fixed));
        for (uint32_t i = 0; i < words; ++i)
            undo->push_back(uint64_t(first + i) << 32 | static_cast<const uint32_t*>(at)[i]);
    }
    bool need(bool changed, uint32_t bit) const
    {
        if (changed)
            return true;
        if (!partial || known(bit))
            return false;
        ++g_extraStateWords[segment != nullptr];
        return true;
    }
    bool known(uint32_t bit) const  { return (m_known[bit >> 6] >> (bit & 63)) & 1; }
    void mark(uint32_t bit)         { m_known[bit >> 6] |= uint64_t(1) << (bit & 63); }

    uint64_t    m_known[(Bits + 63) / 64] = {};
    uint32_t    m_touched = 0;      // transforms (bits 0-3) and viewport (bit 4) logged in this segment
};

// Shadow of effect value blocks for a segment, whose floats start unknown: each float carries the serial that
// last stored it, so a new segment forgets everything by taking a new serial. One shadow serves one segment at a time.
struct ValuesShadow
{
    // Starts a segment: every float is unknown.
    void begin(uint32_t newSerial)
    {
        serial = newSerial;
        touched.clear();
        textures.clear();
        textureWrites = 0;
    }
    // Whether the store changes what the reader has; updates the shadow.
    template< uint32_t N >
    __attribute__((always_inline)) inline bool store(uint32_t block, uint32_t offset, const float* data, uint32_t count)
    {
        if (N)
            count = N;
        if (__builtin_expect(block >= m_blocks.size() || offset + count > m_blocks[block].size(), 0))
            grow(block, offset + count);
        Entry* e = m_blocks[block].data() + offset;
        uint32_t changed = 0, fresh = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t bits;
            memcpy(&bits, &data[i], 4);
            fresh |= e[i].stamp ^ serial;
            changed |= (e[i].stamp ^ serial) | (e[i].bits ^ bits);
            e[i].stamp = serial;
            e[i].bits = bits;
        }
        if (fresh)
            touched.push_back(Range{ uint16_t(block), uint16_t(offset), uint16_t(count) });
        return changed != 0;
    }
    bool store(uint32_t block, uint32_t offset, const float* data, uint32_t count)
    {
        return store<0>(block, offset, data, count);
    }
    // The float's last stored value in this segment, if it stored one.
    bool known(uint32_t block, uint32_t offset, float& value) const
    {
        if (block >= m_blocks.size() || offset >= m_blocks[block].size() || m_blocks[block][offset].stamp != serial)
            return false;
        memcpy(&value, &m_blocks[block][offset].bits, 4);
        return true;
    }

    // Names the context: pending matrix products recorded under another serial record again.
    uint32_t serial = 0;
    struct Range { uint16_t block, offset, count; };
    std::vector<Range> touched;     // first stores of the segment
    // Last value stored at a logged float.
    uint32_t bitsAt(uint32_t block, uint32_t offset) const { return m_blocks[block][offset].bits; }
    struct TextureSet { uint16_t block, param; Object* texture; };
    std::vector<TextureSet> textures;   // effect textures set in this segment
    uint32_t textureWrites = 0;
    Object* const* textureOf(uint32_t block, uint32_t param) const
    {
        for (const TextureSet& t : textures)
            if (t.block == block && t.param == param)
                return &t.texture;
        return nullptr;
    }
    // The texture the parent has at the marker, unless the segment set one.
    void addBase(uint32_t block, uint32_t param, Object* texture)
    {
        if (!textureOf(block, param))
            textures.push_back(TextureSet{ uint16_t(block), uint16_t(param), texture });
    }
    void setTexture(uint32_t block, uint32_t param, Object* texture)
    {
        ++textureWrites;
        for (TextureSet& t : textures)
            if (t.block == block && t.param == param)
            {
                t.texture = texture;
                return;
            }
        textures.push_back(TextureSet{ uint16_t(block), uint16_t(param), texture });
    }

private:
    struct Entry
    {
        uint32_t    bits;
        uint32_t    stamp;      // serial of the segment that stored bits; 0 never
    };
    void grow(uint32_t block, uint32_t floats);
    std::vector<std::vector<Entry>> m_blocks;
};

// Records an effect value store into a segment's stream unless its shadow has the values.
template< uint32_t N >
__attribute__((always_inline)) inline void segmentRecord(Stream& stream, uint32_t id, uint32_t offset, const void* data,
                                                         uint32_t n)
{
    if (N)
        n = N;
    if (!stream.shadow->store<N>(id, offset, static_cast<const float*>(data), n))
        return;
    uint32_t* w = stream.append(2 + n);
    w[0] = CmdValues | id << 8;
    w[1] = offset | n << 16;
    memcpy(w + 2, data, n * 4);
}

// Opens a segment of the frame's recording at this point of the context's stream (a CmdSplice there): the context
// records into the segment, its words unknown. Game thread, between frameStream and the submit.
void beginSegment(RecordContext& ctx, uint32_t frameIndex);
// Closes the segment: the context records into its stream again, its words unknown.
void endSegment(RecordContext& ctx);
// Opens a segment for a clone context of the parent.
// await non-null: the segment is ready later (publishSegmentClone), and the resolver calls await(job) until then.
void beginSegmentClone(RecordContext& parent, RecordContext& clone, uint32_t frameIndex, void (*await)(void*),
                       void* job, uint32_t scene = 0, uint32_t reserved = 0);
// The thread that recorded it: the resolver may read the segment.
void publishSegmentClone(RecordContext& clone, uint32_t draws = 0);
// Game thread, after the publish: the clone's shadow and serial are released.
void endSegmentClone(RecordContext& clone, uint32_t draws = 0);
// Names the context effect stores record into (see ValuesShadow::serial).
uint32_t effectRecordSerial(const Stream* stream);

enum : uint16_t
{
    NoBlock = 0xffff
};
enum : uint8_t
{
    NoEffect = 0xff
};

// The active effect pass of a draw: its kind and value blocks (parameters, shared parameters of its pool).
struct EffectDraw
{
    uint16_t    own, shared;
    uint8_t     kind, pass;     // kind NoEffect: fixed function
    uint8_t     pad[2];
};

struct D3D_STREAM_RECORD DrawRecord
{
    uint32_t                        draw;       // slot in Frame::draws; the recorder wrote its geometry there
    uint8_t                         primitive, layout, texComponents, positionT;
    EffectDraw                      effect;
    const render::TextureStorage*   textures[render::MaxSamplers];  // stage 0, or the pass's samplers
    uint32_t                        deviceSamplers; // effect draws: Device::deviceSamplers() at the draw
    uint32_t                        deviceStates;   // effect draws: Device::deviceStates() at the draw
    const render::BufferStorage*    vertexStorage;  // the vertex buffer the geometry is in; null: frame arena
};

// Render states effect passes set that draws read, in the order of EffectBinding::states.
enum PassState : uint8_t
{
    PassZEnable, PassZWrite, PassAlphaTest, PassSrcBlend, PassDestBlend, PassCull, PassZFunc, PassAlphaRef, PassBlend,
    PassFog, PassFogColor, PassTextureFactor, PassLighting, PassColorWrite, PassStateCount
};
// The D3DRENDERSTATETYPE of each PassState.
extern const uint8_t g_passRenderState[PassStateCount];
// The PassState of a render state, or -1.
int passStateOf(DWORD state);
// The sampler of D3DSAMP states (address modes, filters, mip filter).
render::Sampler samplerOf(const DWORD* states);

// ---- Effects (d3dx9.cpp) -------------------------------------------------------------------------------------------

// What an effect draw draws with: programs, samplers and the render states of the pass over the device's.
struct EffectBinding
{
    uint8_t         vertexProgram;          // render::VertexProgram
    uint8_t         fragmentProgram;
    uint8_t         samplerCount;
    uint8_t         cubeSlots;              // program samplers that are cubes
    bool            skip;                   // nothing to draw with: the draw is dropped
    bool            fixed;                  // fixed-function pass: the device's pipeline with these states
    render::Sampler samplers[render::MaxSamplers];
    DWORD           states[PassStateCount];
};

class EffectPass
{
public:
    virtual ~EffectPass() {}
    // Game thread: false drops the pass's draws unrecorded.
    virtual bool drawn() const = 0;
    // Per draw: pass, sampler textures, slots reading device textures.
    virtual void record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers],
                        uint8_t& deviceSlots) const = 0;

    bool alwaysDrawn = false;   // drawn() is known true for the current pass
};

// Game thread: value blocks changed while no frame recorded, sent whole.
void syncEffectValues(Stream& stream);
// Game thread: every value block sent whole with the next frame.
void markEffectValuesDirty();
// A clone segment overwrote these floats on the stream: the next store of each is sent even if equal.
void forgetSegmentEffects(ValuesShadow& shadow);
// Takes a segment's stores and textures into the effects (a segment on the game thread's own device).
void mergeSegmentEffects(ValuesShadow& shadow);
#ifdef DEV_TOOLS
// Dev: checks segment merges; null when they hold.
const char* effectMergeTest();
#endif
// Render thread: floats of a value block.
uint32_t effectBlockFloats();
// An effect draw's binding, reused while its inputs match.
const EffectBinding& effectBinding(const DrawRecord& record, const FixedState& device, const float* own,
                                   const float* shared, uint32_t version);
// Whether a value block write changes what effectBinding reads.
bool effectBindingReads(uint32_t offset, uint32_t count);
void effectVertexConstants(const DrawRecord& record, const FixedState& device, const float* own,
                           const float* shared, const EffectBinding& binding, float* out);
void effectFragmentConstants(const DrawRecord& record, const float* own, const float* shared,
                             const EffectBinding& binding, float* out);
// Car draws: lowest diffuse alpha car_v outputs, if bounded.
bool effectVertexAlpha(const DrawRecord& record, const float* own, const float* shared, const EffectBinding& binding,
                       float* lowest);
// Sampler stage of a program sampler slot, or -1.
int effectSamplerStage(const EffectDraw& effect, uint32_t slot);
// "IDI_WORLD_FX" (logs).
const char* effectName(const EffectDraw& effect);
// Black cube for cube samplers lacking a cube texture.
const render::TextureStorage* effectBlackCube();
// Render thread, per resolved frame: the effect_stats line.
void effectFrameStats();

// ---- Fixed-function resolution (d3d9_state.cpp, pure) ----------------------------------------------------------------

// Texture stage state indices and values used here.
enum : DWORD
{
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_COUNT = 33,

    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5,
    D3DTOP_MODULATE4X = 6, D3DTOP_ADD = 7,

    D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2, D3DTA_TFACTOR = 3, D3DTA_SELECTMASK = 0xf,

    D3DTTFF_DISABLE = 0, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3, D3DTTFF_COUNT4 = 4, D3DTTFF_PROJECTED = 0x100
};

struct FixedStage
{
    DWORD colorOp, colorArg1, colorArg2;
    DWORD alphaOp, alphaArg1, alphaArg2;
    DWORD transformFlags;
    bool  textured;         // a texture is bound to stage 0
};

struct FixedResult
{
    uint8_t fragmentProgram;    // FpFixedColor, FpFixedTexture or FpFixedProjected
    float   textureMask[4];
    float   scale[4];
    float   addMask[4];
    float   diffuseMask[4];
    float   factorMask[4];
};

// Stage 0 colour and alpha operations as program and masks; unsupported operations are logged and approximated.
FixedResult resolveFixed(const FixedStage& stage);
// Texture coordinate padding for an input of n components: missing components become (.., 1, 0).
void texturePad(uint32_t components, float pad[4]);
// Alpha test constants (sign, threshold) for D3DCMP func and ALPHAREF; false: the test always passes.
bool alphaTest(DWORD func, DWORD ref, float out[2]);

struct BlendFactorDraw
{
    DWORD   src, dst;           // D3DBLEND; BLENDFACTOR / INVBLENDFACTOR replaced
    uint8_t output;             // render::FactorOutput
    float   factor[4];          // uBlendFactor (rgba)
};
// BLENDFACTOR (14) and INVBLENDFACTOR (15) blends as fixed factors and a program factor output. variants: the
// program has factor variants and no alpha test; alphaWrite: the colour mask writes alpha. What is not exact is
// logged and approximated (14 as ONE, 15 as ZERO).
BlendFactorDraw resolveBlendFactor(DWORD src, DWORD dst, DWORD op, DWORD factor, bool variants, bool alphaWrite);
// Row-vector product out = a * b.
void multiply(const float* a, const float* b, float* out);

// ---- Device --------------------------------------------------------------------------------------------------------

enum : DWORD
{
    MaxRenderStates = 256,
    MaxStages = 8,
    MaxSamplerStates = 14,
    MaxSamplerStages = 16
};

// Bits of a stage in Device::deviceSamplers.
enum : uint32_t
{
    DeviceTexture = 1, DeviceAddressU = 2, DeviceAddressV = 4, DeviceMagFilter = 8, DeviceMinFilter = 16,
    DeviceMipFilter = 32
};

class Device : public Object
{
public:
    Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags);
    ~Device() override;

    void setEffectPass(const EffectPass* pass)  { m_effectPass = pass; }
    const EffectPass* effectPass() const        { return m_effectPass; }
    // Stages 0-3, 8 bits each: device writes since BeginPass win.
    uint32_t deviceSamplers() const             { return m_deviceSamplers; }
    // PassStates written since the last BeginPass, one bit each.
    uint32_t deviceStates() const               { return m_deviceStates; }
    void effectPassApplied()                    { m_deviceSamplers = 0; m_deviceStates = 0; }
    void effectTextureApplied(uint32_t stage)   { m_deviceSamplers &= ~(uint32_t(DeviceTexture) << (stage * 8)); }
    // The active effect pass's draws are dropped unrecorded.
    bool effectSkipped() const
    {
        return m_effectPass && !m_effectPass->alwaysDrawn && !m_effectPass->drawn();
    }
    // The stream of the frame being recorded; null between frames.
    Stream* stream() const                      { return m_ctx.stream; }
    // What draws need bound: stream 0 and its stride, indices, a declaration, the FVF.
    void bindings(bool& stream, UINT& stride, bool& indices, bool& declaration, DWORD& fvf) const
    {
        stream = m_state.stream != nullptr;
        stride = m_state.streamStride;
        indices = m_state.indices != nullptr;
        declaration = m_state.declaration != nullptr;
        fvf = m_state.fvf;
    }

    HRESULT TestCooperativeLevel(WinApplication* app, x86::CPU& cpu);
    UINT GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu);
    HRESULT EvictManagedResources(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps);
    HRESULT GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode);
    HRESULT GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params);
    HRESULT SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface);
    void SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags);
    BOOL ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show);
    UINT GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu);
    HRESULT Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params);
    HRESULT Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                    x86::reg32 dirtyRegion);
    HRESULT GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                          x86::reg32* surface);
    HRESULT GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status);
    void SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable);
    void SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp);
    void GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp);
    HRESULT CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                          DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage, DWORD format,
                              DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf, DWORD pool,
                               x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format, DWORD pool,
                              x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                               DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                               x86::reg32 sharedHandle);
    HRESULT CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                      DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                      x86::reg32 sharedHandle);
    HRESULT SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface);
    HRESULT GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface);
    HRESULT StretchRect(WinApplication* app, x86::CPU& cpu, Surface* source, const RECT* sourceRect,
                        Surface* dest, const RECT* destRect, DWORD filter);
    HRESULT SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface);
    HRESULT GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface);
    HRESULT BeginScene(WinApplication* app, x86::CPU& cpu);
    HRESULT EndScene(WinApplication* app, x86::CPU& cpu);
    HRESULT Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags, DWORD color,
                  float z, DWORD stencil);
    HRESULT SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix);
    HRESULT MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport);
    HRESULT GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport);
    HRESULT SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material);
    HRESULT GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material);
    HRESULT SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light);
    HRESULT GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light);
    HRESULT LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable);
    HRESULT GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable);
    HRESULT SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane);
    HRESULT SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value);
    HRESULT GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture);
    HRESULT GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value);
    HRESULT SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value);
    HRESULT GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value);
    HRESULT SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value);
    HRESULT ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes);
    HRESULT SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software);
    BOOL GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu);
    HRESULT SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments);
    HRESULT DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count);
    HRESULT DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                 UINT vertices, UINT startIndex, UINT count);
    // Native callers: memory of the frame being recorded for vertex data (null when the frame's vertex arena is
    // full), and DrawIndexedPrimitive with base vertex 0 that reads the stream's vertices from such memory instead of
    // the stream's buffer; stream stride, declaration and indices as set.
    void* frameVertices(WinApplication* app, x86::CPU& cpu, uint32_t bytes);
    uint32_t frameIndex() const             { return m_frame ? m_frame->index : 0; }
    HRESULT drawIndexedFrameVertices(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                     UINT startIndex, UINT count, const void* data);
    HRESULT DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                            UINT stride);
    HRESULT DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                   UINT count, const void* indices, DWORD indexFormat, const void* data,
                                   UINT stride);
    HRESULT CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                    x86::reg32* declaration);
    HRESULT SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration);
    HRESULT GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration);
    HRESULT SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf);
    HRESULT GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf);
    HRESULT SetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader);
    HRESULT GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);
    HRESULT SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                            UINT stride);
    HRESULT GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                            UINT* stride);
    HRESULT SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider);
    HRESULT SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer);
    HRESULT GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer);
    HRESULT SetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader);
    HRESULT GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);

private:
    struct State
    {
        Object*             textures[MaxSamplerStages];
        VertexDeclaration*  declaration;
        DWORD               fvf;
        Buffer*             stream;
        UINT                streamOffset, streamStride;
        Buffer*             indices;
        uint32_t            layoutStride;   // the draw layout below is of the declaration (or FVF) at this stride
        uint8_t             layoutId, layoutTexComponents;
        bool                layoutPositionT, layoutValid;   // layoutValid: reset when a declaration or FVF is set
        Surface*            target;
        Surface*            depth;
    };

    void defaults();
    void resetBackBuffer(const D3DPRESENT_PARAMETERS& params);
    // Sets a state slot with a binding reference (Object::bind).
    template< typename T > void bindSlot(T*& slot, T* object);
    render::Frame* frame(WinApplication* app, x86::CPU& cpu);
    render::Scene* scene();
    render::Scene* scene(const render::Target& target, uint16_t width, uint16_t height);
    // Allocates from a clone's arena share.
    void* arenaAllocate(uint32_t bytes);
    // False when the frame's viewport table is full (the frame then takes no more draws).
    bool viewportIndex(render::Frame& frame, const render::Viewport& viewport, uint16_t* index);
    bool drawViewport(render::Frame& frame, uint16_t* index);
    // Records a state change for the render thread (header and one value word).
    void record(uint32_t header, DWORD value);
    void recordWords(uint32_t header, const void* payload, uint32_t words);
    void recordDrawCall();
    void submit();
public:
    // Shows the frame recorded so far; false when there is none.
    bool showEarly(WinApplication* app, x86::CPU& cpu, uint32_t* frameIndex);
    // The draws until segmentEnd record into a segment spliced at this point; false with no frame open.
    bool segmentBegin();
    void segmentEnd();
    // A device clone for one segment; null: not opened.
    Device* segmentClone(uint32_t arenaBytes, uint32_t draws, void (*await)(void*), void* job);
    void segmentCloseClone(Device* clone);
    // The thread that ran the clone's segment: publishes it to the resolver.
    void segmentFinishClone(Device* clone);
    uint32_t frameIndexOpen() const { return m_frame ? m_frame->index : 0; }
private:
    explicit Device(Device& parent);
    HRESULT draw(WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices,
                 const uint16_t* indices, uint32_t vertexCount, uint8_t layout, uint32_t texComponents,
                 bool positionT, render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage);
    VertexDeclaration* fvfDeclaration(DWORD fvf);

    // The draw's layout of the bound declaration (or FVF) at a stride; cached under d3d_fast_layer.
    void drawLayout(VertexDeclaration* declaration, uint32_t stride, uint8_t& layout, uint32_t& texComponents,
                    bool& positionT);
    // Marks storages read by the frame being recorded; repeats of the last draw's are skipped (d3d_fast_layer).
    void markRead(render::Frame& frame, const render::TextureStorage* const textures[render::MaxSamplers],
                  render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage);

    WinApplication*         m_app;
    Direct3D*               m_d3d;
    D3DPRESENT_PARAMETERS   m_params;
    HWND                    m_window;
    DWORD                   m_flags;
    std::vector<VertexDeclaration*> m_fvfDeclarations;
    DWORD                   m_lights[8][26] = {};
    BOOL                    m_lightEnabled[8] = {};
    DWORD                   m_material[17] = {};
    RecordContext           m_ctx;          // the recorder state: stream and shadow of the fixed-function state
    State                   m_state;        // from here on: the fields draws read, together
    Surface*                m_backBuffer = nullptr;
    Surface*                m_autoDepth = nullptr;
    const EffectPass*       m_effectPass = nullptr;
    uint32_t                m_deviceSamplers = 0;
    uint32_t                m_deviceStates = 0;

    // Recording
    render::Frame*          m_frame = nullptr;
    bool                    m_sceneOpen = false;    // m_frame's last scene is the current target
    bool                    m_shownEarly = false;   // showEarly submitted the frame the next Present is for
    uint32_t                m_vblanks;
    bool                    m_frameFull = false;
    uint32_t                m_drawCount = 0, m_sceneDraws = 0;   // draws recorded in the frame and its last scene
    uint32_t                m_drawLimit = 0;                     // a clone: the draws the parent reserved for it
    uint32_t                m_segFirst = 0;                      // a clone: its first reserved slot in Frame::draws
    bool                    m_stateDirty = true;    // changed while no frame recorded: sent whole before a draw
    bool                    m_viewportChanged = true;   // m_state.viewport written since drawViewport
    uint32_t                m_viewportFrame = 0, m_viewportCount = 0;   // frame and table size at drawViewport

    bool                    m_fastLayer = false;        // d3d_fast_layer: the caches below and State's layout
    bool                    m_sceneCached = false;      // the last scene is still the one of the bound targets
    uint32_t                m_sceneFrame = 0, m_sceneCount = 0, m_sceneEpoch = 0;   // frame, scene count, texture epoch
    uint32_t                m_markFrame = 0, m_markEpoch = 0;   // m_marked holds what this frame marked at this epoch
    const void*             m_marked[render::MaxSamplers + 2] = {};

    Device*                 m_parent = nullptr;
    uint32_t                m_segFrame = 0;         // a clone: the frame its segment belongs to
    static const uint32_t   MaxClones = 32;
    std::vector<Device*>    m_clones;               // device clones of the segments in flight and spare ones
    bool                    m_segBusy = false;      // a clone: its segment is in flight
    render::Scene*          m_segScene = nullptr;
    uint16_t                m_segViewport = 0;
    uint8_t                 m_segTag = 0;
    uint8_t*                m_arena = nullptr;
    uint32_t                m_arenaUsed = 0, m_arenaSize = 0;
};

}}

namespace win32
{

// Guest COM pointers as host objects in method arguments (null for 0).
#define D3D9_UNPACKER(type)                                                     \
template<> struct Unpacker<type*>                                               \
{                                                                               \
    static inline type* unpack(WinApplication* app, x86::reg32 address)        \
    {                                                                           \
        return static_cast<type*>(d3d9::lookup(app->getMemory<x86::reg32>(address))); \
    }                                                                           \
};

D3D9_UNPACKER(d3d9::Object)
D3D9_UNPACKER(d3d9::Direct3D)
D3D9_UNPACKER(d3d9::Device)
D3D9_UNPACKER(d3d9::Texture)
D3D9_UNPACKER(d3d9::Surface)
D3D9_UNPACKER(d3d9::Buffer)
D3D9_UNPACKER(d3d9::VertexDeclaration)

}

#endif
