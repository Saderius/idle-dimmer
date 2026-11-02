// Native desktop acrylic, hosted by an unpackaged Win32 window.
// References read before implementation:
// Microsoft: https://learn.microsoft.com/windows/uwp/composition/using-the-visual-layer-with-win32
// Microsoft: microsoft-ui-xaml/controls/dev/Materials/Acrylic/AcrylicBrush.cpp (MIT)
// Interop example: https://github.com/ALTaleX531/Win32Acrylic (MIT)
// The host backdrop is already blurred by Windows. There is no screen capture or refresh loop here.
#include "NativeBackdrop.h"
#include "NativeCompositionAbi.h"
#include "AcrylicNoiseData.h"
#include <roapi.h>
#include <winstring.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <uianimation.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

namespace nativebackdrop {
namespace {
using namespace abi;
template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** out() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
};
template<class T> HRESULT As(IUnknown* source, Ref<T>& dest) {
    return source ? source->QueryInterface(T::iid(), (void**)dest.out()) : E_POINTER;
}
struct String {
    HSTRING value = nullptr;
    explicit String(const wchar_t* text) { WindowsCreateString(text, lstrlenW(text), &value); }
    ~String() { WindowsDeleteString(value); }
};
struct Closable : IInspectable {
    static GUID iid() { return Uuid(L"30d5a829-7fa4-4026-83bb-d75bae4ea99e"); }
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
void Close(IUnknown* object) { Ref<Closable> c; if (SUCCEEDED(As(object, c))) c->Close(); }
struct DesktopInterop : IUnknown {
    static GUID iid() { return Uuid(L"29e691fa-4567-4dca-b319-d0f207eb6807"); }
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND, BOOL, IInspectable**) = 0;
};
struct CompositorInterop : IUnknown {
    static GUID iid() { return Uuid(L"25297d5c-3ad4-4c9c-b5cf-e36a38512330"); }
    virtual HRESULT STDMETHODCALLTYPE unused_CreateCompositionSurfaceForHandle() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForSwapChain(IUnknown*, IInspectable**) = 0;
};
struct AsyncInfo : IInspectable {
    static GUID iid() { return Uuid(L"00000036-0000-0000-c000-000000000046"); }
    virtual HRESULT STDMETHODCALLTYPE unused_Id() = 0;
    virtual HRESULT STDMETHODCALLTYPE Status(int*) = 0;
};

// The effect description is passed to Windows, which builds/runs the graph on the compositor.
// These are the documented IGraphicsEffect / IGraphicsEffectD2D1Interop ABIs, not a CPU renderer.
}
namespace abi {
struct IGraphicsEffect : IInspectable {
    static GUID iid() { return Uuid(L"cb51c0ce-8fe6-4636-b202-861faa07d8f3"); }
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Name(HSTRING) = 0;
};
}
namespace {
struct EffectInterop : IUnknown {
    static GUID iid() { return Uuid(L"2fc57384-a068-44d7-a331-30982fcf7177"); }
    virtual HRESULT STDMETHODCALLTYPE GetEffectId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UINT, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSource(UINT, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceCount(UINT*) = 0;
};
class Effect final : public IGraphicsEffect, public EffectInterop {
    LONG refs = 1;
    GUID id;
    IInspectable* properties[3] = {};
    IInspectable* sources[2] = {};
    UINT propertyCount = 0, sourceCount = 0;
public:
    static void* operator new(size_t bytes) noexcept { return HeapAlloc(GetProcessHeap(), 0, bytes); }
    static void operator delete(void* value) noexcept { HeapFree(GetProcessHeap(), 0, value); }
    explicit Effect(GUID effectId) : id(effectId) {}
    ~Effect() {
        for (auto p : properties) if (p) p->Release();
        for (auto p : sources) if (p) p->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == Uuid(L"af86e2e0-b12d-4c6a-9c5a-d7aa65101e90") || iid == IGraphicsEffect::iid() ||
            iid == Uuid(L"2d8f9ddc-4339-4eb9-9216-f9deb75658a2"))
            *result = static_cast<IGraphicsEffect*>(this);
        else if (iid == EffectInterop::iid()) *result = static_cast<EffectInterop*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG remaining = InterlockedDecrement(&refs); if (!remaining) delete this; return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** ids) override {
        if (!count || !ids) return E_POINTER; *count = 0; *ids = nullptr; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
        if (!name) return E_POINTER; *name = nullptr; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* value) override {
        if (!value) return E_POINTER; *value = BaseTrust; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Name(HSTRING* name) override { return GetRuntimeClassName(name); }
    HRESULT STDMETHODCALLTYPE put_Name(HSTRING) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* value) override {
        if (!value) return E_POINTER; *value = id; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*, int*) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) override {
        if (!count) return E_POINTER; *count = propertyCount; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) override {
        if (!count) return E_POINTER; *count = sourceCount; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index, IInspectable** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (index >= propertyCount || !properties[index]) return E_INVALIDARG;
        *value = properties[index]; (*value)->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index, IInspectable** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (index >= sourceCount || !sources[index]) return E_INVALIDARG;
        *value = sources[index]; (*value)->AddRef(); return S_OK;
    }
    void Property(UINT index, IInspectable* value) {
        if (index >= 3 || !value) return;
        if (properties[index]) properties[index]->Release();
        properties[index] = value; value->AddRef();
        if (propertyCount <= index) propertyCount = index + 1;
    }
    void Source(UINT index, IInspectable* value) {
        if (index >= 2 || !value) return;
        if (sources[index]) sources[index]->Release();
        sources[index] = value; value->AddRef();
        if (sourceCount <= index) sourceCount = index + 1;
    }
};

struct State {
    HANDLE thread = nullptr, ready = nullptr, stop = nullptr, update = nullptr, motion = nullptr;
    CRITICAL_SECTION lock;
    bool lockReady = false;
    HWND panel = nullptr;
    LONG_PTR originalStyle = 0;
    bool styleChanged = false;
    bool cloaked = false;
    int anchorX = 0, anchorY = 0;
    int width = 0, height = 0, dpi = 96;
    bool dark = true;
    bool animate = false;
    BYTE* pixels = nullptr;
    volatile LONG active = 0, frames = 0;
    volatile LONG firstFrameReady = 0;
    volatile LONG session = 0, requestedMotion = 0, motionPhase = 0, completedAnimations = 0;
    HRESULT motionError = S_OK;
    const wchar_t* motionStage = L"not requested";
    HRESULT error = S_OK;
    const wchar_t* stage = L"not started";
} g;

// Follows Microsoft's WindowsAnimation/TimerDriven sample. Animate the HWND, not its client
// subtree: otherwise DWM exposes an empty stationary border/shadow before content arrives.
struct WindowMotion final : IUIAnimationTimerEventHandler {
    LONG refs = 1;
    Ref<IUIAnimationManager> manager;
    Ref<IUIAnimationTimer> timer;
    Ref<IUIAnimationTransitionLibrary> library;
    Ref<IUIAnimationVariable> position;
    bool active = false, hiding = false;
    unsigned session = 0;
    int finalY = 0;
    static void* operator new(size_t bytes) noexcept { return HeapAlloc(GetProcessHeap(), 0, bytes); }
    static void operator delete(void* p) noexcept { HeapFree(GetProcessHeap(), 0, p); }
    ~WindowMotion() { Cancel(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IUIAnimationTimerEventHandler)) return E_NOINTERFACE;
        *result = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&refs); if (!count) delete this; return count;
    }
    void Cancel() {
        active = false;
        if (timer) {
            timer->Disable();
            timer->SetTimerEventHandler(nullptr);
            timer->SetTimerUpdateHandler(nullptr, UI_ANIMATION_IDLE_BEHAVIOR_DISABLE);
        }
        if (manager) { manager->AbandonAllStoryboards(); manager->Shutdown(); }
        position.reset(); library.reset(); timer.reset(); manager.reset();
    }
    HRESULT Begin(bool close) {
        Cancel();
        hiding = close; session = Session();
        finalY = g.anchorY + (hiding ? MulDiv(kNativeSlideTravel, g.dpi, 96) : 0);
        RECT rect = {};
        if (!GetWindowRect(g.panel, &rect)) return HRESULT_FROM_WIN32(GetLastError());
        g.motionStage = L"Windows Animation Manager";
        HRESULT hr = CoCreateInstance(__uuidof(UIAnimationManager), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IUIAnimationManager), (void**)manager.out());
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(UIAnimationTimer), nullptr, CLSCTX_INPROC_SERVER,
                                                 __uuidof(IUIAnimationTimer), (void**)timer.out());
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(UIAnimationTransitionLibrary), nullptr, CLSCTX_INPROC_SERVER,
                                                 __uuidof(IUIAnimationTransitionLibrary), (void**)library.out());
        Ref<IUIAnimationTimerUpdateHandler> update;
        if (SUCCEEDED(hr)) hr = manager->QueryInterface(__uuidof(IUIAnimationTimerUpdateHandler), (void**)update.out());
        if (SUCCEEDED(hr)) hr = timer->SetTimerUpdateHandler(update.p, UI_ANIMATION_IDLE_BEHAVIOR_DISABLE);
        if (SUCCEEDED(hr)) hr = timer->SetTimerEventHandler(this);
        if (SUCCEEDED(hr)) hr = manager->CreateAnimationVariable((double)rect.top, position.out());
        Ref<IUIAnimationStoryboard> storyboard;
        Ref<IUIAnimationTransition> transition;
        if (SUCCEEDED(hr)) hr = manager->CreateStoryboard(storyboard.out());
        const double duration = (hiding ? kNativeSlideExitMs : kNativeSlideEntranceMs) / 1000.0;
        if (SUCCEEDED(hr)) hr = library->CreateAccelerateDecelerateTransition(
            duration, (double)finalY, hiding ? 1.0 : 0.0, hiding ? 0.0 : 1.0, transition.out());
        if (SUCCEEDED(hr)) hr = storyboard->AddTransition(position.p, transition.p);
        if (SUCCEEDED(hr) && !hiding && !Reveal()) hr = g.motionError;
        double now = 0;
        if (SUCCEEDED(hr)) hr = timer->GetTime(&now);
        if (SUCCEEDED(hr)) {
            active = true;
            g.motionStage = L"whole-window storyboard";
            hr = storyboard->Schedule(now);
        }
        if (FAILED(hr)) {
            Cancel();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnPreUpdate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnRenderingTooSlow(UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPostUpdate() override {
        if (!active || session != Session()) return S_OK;
        double value = 0;
        HRESULT hr = position->GetValue(&value);
        UI_ANIMATION_MANAGER_STATUS status = UI_ANIMATION_MANAGER_BUSY;
        if (SUCCEEDED(hr)) hr = manager->GetStatus(&status);
        const bool done = FAILED(hr) || status == UI_ANIMATION_MANAGER_IDLE;
        const int y = done ? finalY : (int)(value >= 0 ? value + 0.5 : value - 0.5);
        // Post position to UI thread: single position authority tagged by session.
        // Avoid cross-thread SetWindowPos(SWP_ASYNCWINDOWPOS) racing with UI-thread positioning.
        PostMessageW(g.panel, kAnimationUpdateMessage, ((WPARAM)session<<1)|(hiding?1:0), (LPARAM)y);
        if (done) {
            active = false;
            timer->Disable();
            g.motionError = hr;
            InterlockedExchange(&g.motionPhase, 0);
            InterlockedIncrement(&g.completedAnimations);
            PostMessageW(g.panel, kAnimationCompleteMessage, hiding ? 1 : 0, (LPARAM)session);
        }
        return S_OK;
    }
};

#define NATIVE_TRY(label, expression) do { g.stage = label; HRESULT result_ = (expression); \
    if (FAILED(result_)) return result_; } while (0)

struct Scene {
    Ref<ICompositor> compositor;
    Ref<IInspectable> targetObject;
    Ref<ICompositionTarget> target;
    Ref<ISpriteVisual> background, controls;
    Ref<IVisual> backgroundVisual, controlsVisual;
    Ref<ICompositionEffectBrush> material;
    Ref<ICompositionBrush> materialBrush, controlsBrush;
    Ref<IInspectable> controlsSurface;
    Ref<ID3D11Device> device;
    Ref<ID3D11DeviceContext> context;
    Ref<IDXGIFactory2> factory;
    Ref<IDXGISwapChain1> controlsChain, noiseChain;
    Ref<WindowMotion> motion;
    ~Scene() {
        if (motion) motion->Cancel();
        if (target) target->put_Root(nullptr);
        Close(targetObject.p);
        Close(compositor.p);
        if (context) { context->ClearState(); context->Flush(); }
    }
    HRESULT Slide(bool hiding) {
        if (!motion) motion.p = new WindowMotion();
        HRESULT hr = motion ? motion->Begin(hiding) : E_OUTOFMEMORY;
        if (FAILED(hr)) {
            if (!hiding) {
                Reveal();
                // A failed storyboard still reaches the resting position through the UI's
                // normal update path before it receives completion.
                PostMessageW(g.panel, kAnimationUpdateMessage, (WPARAM)Session()<<1, (LPARAM)g.anchorY);
            }
            InterlockedExchange(&g.motionPhase, 0);
            PostMessageW(g.panel, kAnimationCompleteMessage, hiding ? 1 : 0, (LPARAM)Session());
        }
        return hr;
    }
    bool PumpPreparation(ULONGLONG deadline) {
        if (GetTickCount64() >= deadline) return false;
        if (MsgWaitForMultipleObjectsEx(1, &g.stop, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0)
            return false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        return true;
    }
    HRESULT PrepareFirstFrame() {
        // Present1 queues work. Wait for the uploaded control/noise pixels to finish on the GPU
        // before committing the tree; an accepted Present1 alone is not a complete first frame.
        Ref<ID3D11Query> fence;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
        NATIVE_TRY(L"first-frame GPU fence", device->CreateQuery(&desc, fence.out()));
        context->End(fence.p);
        context->Flush();
        const ULONGLONG deadline = GetTickCount64() + 2000;
        for (;;) {
            BOOL finished = FALSE;
            HRESULT hr = context->GetData(fence.p, &finished, sizeof(finished), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(hr)) return hr;
            if (hr == S_OK && finished) break;
            if (!PumpPreparation(deadline)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        Ref<ICompositor5> commit;
        Ref<IInspectable> operation;
        Ref<AsyncInfo> info;
        NATIVE_TRY(L"first-frame commit interface", As(compositor.p, commit));
        NATIVE_TRY(L"first-frame composition commit", commit->RequestCommitAsync(operation.out()));
        NATIVE_TRY(L"first-frame commit status", As(operation.p, info));
        for (;;) {
            int status = 0;
            HRESULT hr = info->Status(&status);
            if (FAILED(hr)) return hr;
            if (status == 1) return S_OK;
            if (status != 0) return E_ABORT;
            if (!PumpPreparation(deadline)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }    HRESULT Chain(int width, int height, Ref<IDXGISwapChain1>& chain) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width; desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        return factory->CreateSwapChainForComposition(device.p, &desc, nullptr, chain.out());
    }
    HRESULT Upload(IDXGISwapChain1* chain, const void* pixels, int width) {
        Ref<ID3D11Texture2D> buffer;
        HRESULT hr = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)buffer.out());
        if (FAILED(hr)) return hr;
        context->UpdateSubresource(buffer.p, 0, nullptr, pixels, width * 4, 0);
        DXGI_PRESENT_PARAMETERS params = {};
        return chain->Present1(0, 0, &params);
    }
    HRESULT SurfaceBrush(IDXGISwapChain1* chain, Ref<ICompositionBrush>& brush) {
        Ref<CompositorInterop> interop;
        Ref<IInspectable> surface, surfaceBrush;
        HRESULT hr = As(compositor.p, interop);
        if (SUCCEEDED(hr)) hr = interop->CreateCompositionSurfaceForSwapChain(chain, surface.out());
        if (SUCCEEDED(hr)) hr = compositor->CreateSurfaceBrushWithSurface(surface.p, surfaceBrush.out());
        if (SUCCEEDED(hr) && chain == noiseChain.p) {
            Ref<ICompositionSurfaceBrush> noise;
            hr = As(surfaceBrush.p, noise);
            if (SUCCEEDED(hr)) hr = noise->put_Stretch(0); // CompositionStretch.None: grain stays 1:1
            if (SUCCEEDED(hr)) hr = noise->put_HorizontalAlignmentRatio(0.0f);
            if (SUCCEEDED(hr)) hr = noise->put_VerticalAlignmentRatio(0.0f);
        }
        if (SUCCEEDED(hr)) hr = As(surfaceBrush.p, brush);
        return hr;
    }
    HRESULT Noise() {
        // Original Microsoft MIT-licensed acrylic texture. No random stand-in or resized grain.
        Ref<IWICImagingFactory> wic;
        Ref<IStream> stream;
        Ref<IWICBitmapDecoder> decoder;
        Ref<IWICBitmapFrameDecode> frame;
        Ref<IWICFormatConverter> converter;
        NATIVE_TRY(L"noise decoder", CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_IWICImagingFactory, (void**)wic.out()));
        HGLOBAL encoded = GlobalAlloc(GMEM_MOVEABLE, sizeof(acrylicnoise::kPng));
        if (!encoded) return E_OUTOFMEMORY;
        void* encodedBytes = GlobalLock(encoded);
        if (!encodedBytes) { GlobalFree(encoded); return E_OUTOFMEMORY; }
        memcpy(encodedBytes, acrylicnoise::kPng, sizeof(acrylicnoise::kPng));
        GlobalUnlock(encoded);
        HRESULT streamResult = CreateStreamOnHGlobal(encoded, TRUE, stream.out());
        if (FAILED(streamResult)) { GlobalFree(encoded); return streamResult; }
        NATIVE_TRY(L"noise asset", wic->CreateDecoderFromStream(stream.p, nullptr,
                                                             WICDecodeMetadataCacheOnLoad, decoder.out()));
        NATIVE_TRY(L"noise frame", decoder->GetFrame(0, frame.out()));
        UINT width = 0, height = 0;
        NATIVE_TRY(L"noise size", frame->GetSize(&width, &height));
        if (width != 256 || height != 256) return E_INVALIDARG;
        NATIVE_TRY(L"noise converter", wic->CreateFormatConverter(converter.out()));
        NATIVE_TRY(L"noise format", converter->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA,
                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
        BYTE* data = (BYTE*)HeapAlloc(GetProcessHeap(), 0, 256 * 256 * 4);
        if (!data) return E_OUTOFMEMORY;
        HRESULT hr = converter->CopyPixels(nullptr, 256 * 4, 256 * 256 * 4, data);
        if (SUCCEEDED(hr)) hr = Chain(256, 256, noiseChain);
        if (SUCCEEDED(hr)) hr = Upload(noiseChain.p, data, 256);
        HeapFree(GetProcessHeap(), 0, data);
        return hr;
    }
    HRESULT Material() {
        Ref<IPropertyValueStatics> values;
        Ref<ICompositionEffectSourceParameterFactory> sources;
        String valueClass(L"Windows.Foundation.PropertyValue");
        String sourceClass(L"Windows.UI.Composition.CompositionEffectSourceParameter");
        NATIVE_TRY(L"effect property factory", RoGetActivationFactory(valueClass.value, IPropertyValueStatics::iid(), (void**)values.out()));
        NATIVE_TRY(L"effect source factory", RoGetActivationFactory(sourceClass.value, ICompositionEffectSourceParameterFactory::iid(), (void**)sources.out()));
        String backdropName(L"Backdrop"), noiseName(L"Noise");
        Ref<IInspectable> backdropSource, noiseSource;
        NATIVE_TRY(L"backdrop source", sources->Create(backdropName.value, backdropSource.out()));
        NATIVE_TRY(L"noise source", sources->Create(noiseName.value, noiseSource.out()));

        // Published Base acrylic preset, not the former mix of Base/Default/legacy formulas.
        const float tint = g.dark ? 32.0f / 255.0f : 243.0f / 255.0f;
        float tintColor[4] = {tint, tint, tint, g.dark ? 0.5f : 0.0f};
        float luminosityColor[4] = {tint, tint, tint, g.dark ? 0.96f : 0.90f};
        Ref<IInspectable> tintValue, luminosityValue, colorMode, luminosityMode, borderMode, noiseOpacity, compositeMode;
        NATIVE_TRY(L"tint property", values->CreateSingleArray(4, tintColor, tintValue.out()));
        NATIVE_TRY(L"luminosity property", values->CreateSingleArray(4, luminosityColor, luminosityValue.out()));
        // Windows Composition's Color/Luminosity enum names are swapped; see AcrylicBrush.cpp.
        NATIVE_TRY(L"luminosity mode", values->CreateUInt32(D2D1_BLEND_MODE_COLOR, luminosityMode.out()));
        NATIVE_TRY(L"color mode", values->CreateUInt32(D2D1_BLEND_MODE_LUMINOSITY, colorMode.out()));
        NATIVE_TRY(L"noise wrap", values->CreateUInt32(D2D1_BORDER_EDGE_MODE_WRAP, borderMode.out()));
        NATIVE_TRY(L"noise opacity", values->CreateSingle(0.02f, noiseOpacity.out()));
        NATIVE_TRY(L"noise composition", values->CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER, compositeMode.out()));
        Ref<Effect> tintEffect, luminosityEffect, luminosity, color, border, opacity, composite;
        tintEffect.p = new Effect(CLSID_D2D1Flood); luminosityEffect.p = new Effect(CLSID_D2D1Flood);
        luminosity.p = new Effect(CLSID_D2D1Blend); color.p = new Effect(CLSID_D2D1Blend);
        border.p = new Effect(CLSID_D2D1Border); opacity.p = new Effect(CLSID_D2D1Opacity);
        composite.p = new Effect(CLSID_D2D1Composite);
        if (!tintEffect || !luminosityEffect || !luminosity || !color || !border || !opacity || !composite)
            return E_OUTOFMEMORY;
        tintEffect->Property(0, tintValue.p); luminosityEffect->Property(0, luminosityValue.p);
        luminosity->Property(0, luminosityMode.p);
        luminosity->Source(0, backdropSource.p); luminosity->Source(1, luminosityEffect.p);
        color->Property(0, colorMode.p);
        color->Source(0, luminosity.p); color->Source(1, tintEffect.p);
        border->Property(0, borderMode.p); border->Property(1, borderMode.p); border->Source(0, noiseSource.p);
        opacity->Property(0, noiseOpacity.p); opacity->Source(0, border.p);
        composite->Property(0, compositeMode.p); composite->Source(0, color.p); composite->Source(1, opacity.p);
        Ref<ICompositionEffectFactory> effectFactory;
        NATIVE_TRY(L"acrylic effect graph", compositor->CreateEffectFactory(composite.p, effectFactory.out()));
        NATIVE_TRY(L"acrylic brush", effectFactory->CreateBrush(material.out()));
        Ref<ICompositor3> compositor3;
        Ref<IInspectable> backdropObject;
        Ref<ICompositionBrush> backdropBrush, noiseBrush;
        NATIVE_TRY(L"host backdrop interface", As(compositor.p, compositor3));
        NATIVE_TRY(L"host backdrop", compositor3->CreateHostBackdropBrush(backdropObject.out()));
        NATIVE_TRY(L"host backdrop brush", As(backdropObject.p, backdropBrush));
        NATIVE_TRY(L"noise surface brush", SurfaceBrush(noiseChain.p, noiseBrush));
        NATIVE_TRY(L"backdrop binding", material->SetSourceParameter(backdropName.value, backdropBrush.p));
        NATIVE_TRY(L"noise binding", material->SetSourceParameter(noiseName.value, noiseBrush.p));
        return As(material.p, materialBrush);
    }
    HRESULT Initialize() {
        String cls(L"Windows.UI.Composition.Compositor");
        Ref<IInspectable> instance;
        NATIVE_TRY(L"compositor activation", RoActivateInstance(cls.value, instance.out()));
        NATIVE_TRY(L"compositor interface", As(instance.p, compositor));
        Ref<DesktopInterop> desktop;
        NATIVE_TRY(L"desktop interop", As(compositor.p, desktop));
        NATIVE_TRY(L"desktop target", desktop->CreateDesktopWindowTarget(g.panel, TRUE, targetObject.out()));
        NATIVE_TRY(L"composition target", As(targetObject.p, target));
        NATIVE_TRY(L"D3D device", D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.out(), nullptr, context.out()));
        Ref<IDXGIDevice> dxgi;
        Ref<IDXGIAdapter> adapter;
        NATIVE_TRY(L"DXGI device", device->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgi.out()));
        NATIVE_TRY(L"DXGI adapter", dxgi->GetAdapter(adapter.out()));
        NATIVE_TRY(L"DXGI factory", adapter->GetParent(__uuidof(IDXGIFactory2), (void**)factory.out()));
        NATIVE_TRY(L"control swapchain", Chain(g.width, g.height, controlsChain));
        NATIVE_TRY(L"initial controls", Upload(controlsChain.p, g.pixels, g.width));
        NATIVE_TRY(L"native noise", Noise());
        NATIVE_TRY(L"native material", Material());
        NATIVE_TRY(L"background visual", compositor->CreateSpriteVisual(background.out()));
        NATIVE_TRY(L"background geometry", As(background.p, backgroundVisual));
        NATIVE_TRY(L"background size", backgroundVisual->put_Size({(float)g.width, (float)g.height}));
        NATIVE_TRY(L"initial opacity", backgroundVisual->put_Opacity(1.0f));
        NATIVE_TRY(L"initial client position", backgroundVisual->put_Offset({0.0f, 0.0f, 0.0f}));
        NATIVE_TRY(L"background brush", background->put_Brush(materialBrush.p));
        NATIVE_TRY(L"control visual", compositor->CreateSpriteVisual(controls.out()));
        NATIVE_TRY(L"control geometry", As(controls.p, controlsVisual));
        NATIVE_TRY(L"control size", controlsVisual->put_Size({(float)g.width, (float)g.height}));
        NATIVE_TRY(L"control surface", SurfaceBrush(controlsChain.p, controlsBrush));
        NATIVE_TRY(L"control brush", controls->put_Brush(controlsBrush.p));
        Ref<IContainerVisual> container;
        Ref<IVisualCollection> children;
        NATIVE_TRY(L"visual container", As(background.p, container));
        NATIVE_TRY(L"visual children", container->get_Children(children.out()));
        NATIVE_TRY(L"control attachment", children->InsertAtTop(controlsVisual.p));
        NATIVE_TRY(L"root attachment", target->put_Root(backgroundVisual.p));
        NATIVE_TRY(L"prepared first frame", PrepareFirstFrame());
        InterlockedExchange(&g.firstFrameReady, 1);
        return S_OK;
    }
};

void DrainQueue(IDispatcherQueueController* queue) {
    if (!queue) return;
    Ref<IInspectable> operation;
    if (FAILED(queue->ShutdownQueueAsync(operation.out()))) return;
    Ref<AsyncInfo> info;
    if (FAILED(As(operation.p, info))) return;
    // Shutdown needs its own message pump. This dedicated thread never dispatches the app's UI.
    const ULONGLONG deadline = GetTickCount64() + 5000;
    int status = 0;
    while (SUCCEEDED(info->Status(&status)) && status == 0 && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
}
DWORD WINAPI RenderThread(void*) {
    HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
    if (desktop) SetThreadDesktop(desktop);
    g.stage = L"WinRT apartment";
    HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
    if (SUCCEEDED(hr)) {
        Ref<IDispatcherQueueController> queue;
        HMODULE core = LoadLibraryW(L"CoreMessaging.dll");
        struct Options { DWORD size; int thread; int apartment; };
        using CreateQueue = HRESULT(WINAPI*)(Options, IDispatcherQueueController**);
        auto create = core ? (CreateQueue)GetProcAddress(core, "CreateDispatcherQueueController") : nullptr;
        g.stage = L"DispatcherQueue";
        hr = create ? create({sizeof(Options), 2, 2}, queue.out()) : E_NOINTERFACE;
        {
            Scene scene;
            if (SUCCEEDED(hr)) hr = scene.Initialize();
            g.error = hr;
            if (SUCCEEDED(hr)) {
                g.stage = L"native host backdrop";
                InterlockedExchange(&g.active, 1);
                InterlockedIncrement(&g.frames);
            }
            SetEvent(g.ready);
            if (SUCCEEDED(hr)) {
                HANDLE events[3] = {g.stop, g.motion, g.update};
                for (;;) {
                    DWORD wait = MsgWaitForMultipleObjectsEx(3, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (wait == WAIT_OBJECT_0) break;
                    if (wait == WAIT_FAILED) {
                        g.error = HRESULT_FROM_WIN32(GetLastError()); g.stage = L"native message wait";
                        PostMessageW(g.panel, kFailureMessage, 0, 0); break;
                    }
                    if (wait == WAIT_OBJECT_0 + 1) {
                        const LONG request = InterlockedExchange(&g.requestedMotion, 0);
                        if (request) g.motionError = scene.Slide(request == 2);
                    }
                    if (wait == WAIT_OBJECT_0 + 2) {
                        EnterCriticalSection(&g.lock);
                        HRESULT paint = scene.Upload(scene.controlsChain.p, g.pixels, g.width);
                        LeaveCriticalSection(&g.lock);
                        if (FAILED(paint)) {
                            g.error = paint; g.stage = L"control presentation";
                            PostMessageW(g.panel, kFailureMessage, 0, 0); break;
                        }
                        InterlockedIncrement(&g.frames);
                    }
                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg); DispatchMessageW(&msg);
                    }
                }
            }
            InterlockedExchange(&g.active, 0);
        } // detach and close the compositor, and release GPU resources, before draining its queue
        DrainQueue(queue.p);
        queue.reset();
        if (core) FreeLibrary(core);
        RoUninitialize();
    } else { g.error = hr; SetEvent(g.ready); }
    if (desktop) CloseDesktop(desktop);
    return 0;
}
} // namespace

bool Allowed() {
    HIGHCONTRASTW contrast = {sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;
    SYSTEM_POWER_STATUS power = {};
    if (GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1) return false;
    DWORD transparency = 1, bytes = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &bytes);
    return transparency != 0;
}
bool Start(HWND panel, int width, int height, bool dark, int dpi, const void* pixels, bool animate) {
    Stop();
    if (g.thread || !panel || width <= 0 || height <= 0 || !pixels) return false;
    if (!g.lockReady) { InitializeCriticalSection(&g.lock); g.lockReady = true; }
    g.panel = panel; g.width = width; g.height = height; g.dark = dark; g.dpi = dpi;
    RECT anchor = {};
    if (!GetWindowRect(panel, &anchor)) return false;
    g.anchorX = anchor.left; g.anchorY = anchor.top;
    g.animate = animate;
    InterlockedExchange(&g.firstFrameReady, 0);
    InterlockedIncrement(&g.session);
    InterlockedExchange(&g.requestedMotion, 0);
    InterlockedExchange(&g.motionPhase, 0);
    g.motionError = S_OK;
    g.motionStage = L"not requested";
    g.error = S_OK;
    g.pixels = (BYTE*)HeapAlloc(GetProcessHeap(), 0, (size_t)width * height * 4);
    g.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.update = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.motion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.pixels || !g.ready || !g.stop || !g.update || !g.motion) { g.error = E_OUTOFMEMORY; Stop(); return false; }
    memcpy(g.pixels, pixels, (size_t)width * height * 4);
    // Keep the actual DWM border/shadow hidden until the complete native tree has committed.
    const BOOL yes = TRUE;
    HRESULT cloak = DwmSetWindowAttribute(panel, 13, &yes, sizeof(yes)); // DWMWA_CLOAK
    if (FAILED(cloak)) { g.error = cloak; g.stage = L"preparation cloak"; Stop(); return false; }
    g.cloaked = true;
    DwmSetWindowAttribute(panel, 3, &yes, sizeof(yes)); // prevent a second DWM show transition
    g.originalStyle = GetWindowLongPtrW(panel, GWL_EXSTYLE);
    g.styleChanged = true;
    SetWindowLongPtrW(panel, GWL_EXSTYLE, g.originalStyle | WS_EX_NOREDIRECTIONBITMAP);
    // Host backdrop access is explicitly enabled for this non-UWP window (Win11).
    const BOOL hostBackdrop = TRUE;
    const int noSystemPlate = 1;
    DwmSetWindowAttribute(panel, 38, &noSystemPlate, sizeof(noSystemPlate));
    HRESULT hostResult = DwmSetWindowAttribute(panel, 17, &hostBackdrop, sizeof(hostBackdrop));
    if (FAILED(hostResult)) { g.error = hostResult; g.stage = L"host backdrop permission"; Stop(); return false; }
    g.thread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
    if (!g.thread) { g.error = HRESULT_FROM_WIN32(GetLastError()); Stop(); return false; }
    if (WaitForSingleObject(g.ready, 10000) != WAIT_OBJECT_0) {
        g.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT); g.stage = L"native startup timeout"; Stop(); return false;
    }
    if (!Running()) { Stop(); return false; }
    return true;
}
bool Update(const void* pixels, int width, int height) {
    if (!Running() || !pixels || width != g.width || height != g.height) return false;
    EnterCriticalSection(&g.lock);
    memcpy(g.pixels, pixels, (size_t)width * height * 4);
    LeaveCriticalSection(&g.lock);
    SetEvent(g.update);
    return true;
}
bool AnimateShow() {
    if (!Running() || !g.animate) return false;
    InterlockedExchange(&g.motionPhase, 1);
    InterlockedExchange(&g.requestedMotion, 1);
    if (SetEvent(g.motion)) return true;
    g.motionError = HRESULT_FROM_WIN32(GetLastError()); return false;
}
bool AnimateHide() {
    if (!Running() || !g.animate) return false;
    InterlockedExchange(&g.motionPhase, 2);
    InterlockedExchange(&g.requestedMotion, 2);
    if (SetEvent(g.motion)) return true;
    g.motionError = HRESULT_FROM_WIN32(GetLastError()); return false;
}
bool Reveal() {
    const BOOL no = FALSE;
    HRESULT hr = DwmSetWindowAttribute(g.panel, 13, &no, sizeof(no));
    if (FAILED(hr)) { g.motionError = hr; g.motionStage = L"prepared-window reveal"; return false; }
    g.cloaked = false;
    return true;
}
void Stop() {
    if (g.thread) {
        SetEvent(g.stop);
        if (WaitForSingleObject(g.thread, 10000) != WAIT_OBJECT_0) {
            g.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT); g.stage = L"native shutdown timeout"; return;
        }
        CloseHandle(g.thread); g.thread = nullptr;
    }
    if (g.ready) { CloseHandle(g.ready); g.ready = nullptr; }
    if (g.stop) { CloseHandle(g.stop); g.stop = nullptr; }
    if (g.update) { CloseHandle(g.update); g.update = nullptr; }
    if (g.motion) { CloseHandle(g.motion); g.motion = nullptr; }
    if (g.pixels) { HeapFree(GetProcessHeap(), 0, g.pixels); g.pixels = nullptr; }
    if (g.styleChanged && IsWindow(g.panel)) {
        SetWindowLongPtrW(g.panel, GWL_EXSTYLE, g.originalStyle);
        const BOOL disabled = FALSE;
        DwmSetWindowAttribute(g.panel, 17, &disabled, sizeof(disabled));
    }
    g.styleChanged = false;
    if (IsWindow(g.panel)) {
        const BOOL no = FALSE;
        DwmSetWindowAttribute(g.panel, 3, &no, sizeof(no));
        if (g.cloaked) DwmSetWindowAttribute(g.panel, 13, &no, sizeof(no));
    }
    g.cloaked = false;
    InterlockedExchange(&g.active, 0);
    InterlockedExchange(&g.motionPhase, 0);
    InterlockedExchange(&g.firstFrameReady, 0);
}
bool Running() { return InterlockedCompareExchange(&g.active, 0, 0) != 0; }
bool FirstFrameReady() { return InterlockedCompareExchange(&g.firstFrameReady, 0, 0) != 0; }
bool Cloaked() { return g.cloaked; }
bool WorkerAlive() { return g.thread && WaitForSingleObject(g.thread, 0) == WAIT_TIMEOUT; }
HRESULT LastError() { return g.error; }
const wchar_t* Stage() { return g.stage; }
unsigned Frames() { return (unsigned)InterlockedCompareExchange(&g.frames, 0, 0); }
unsigned Session() { return (unsigned)InterlockedCompareExchange(&g.session, 0, 0); }
const wchar_t* AnimationPhase() {
    const LONG phase = InterlockedCompareExchange(&g.motionPhase, 0, 0);
    return phase == 1 ? L"entering" : (phase == 2 ? L"exiting" : L"idle");
}
HRESULT AnimationError() { return g.motionError; }
const wchar_t* AnimationStage() { return g.motionStage; }
unsigned CompletedAnimations() { return (unsigned)InterlockedCompareExchange(&g.completedAnimations, 0, 0); }
} // namespace nativebackdrop
