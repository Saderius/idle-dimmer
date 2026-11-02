// Native Windows tray app. No framework, managed runtime, network, or worker polling loop.
// Build with build.ps1. Media ABI follows Microsoft's Windows.Media.Control interfaces.
// Theme handling follows documented Windows behavior: HKCU\...\Themes\Personalize\SystemUsesLightTheme
// (light/dark for Windows), WM_SETTINGCHANGE "ImmersiveColorSet", separate light/dark icon assets per
// Fluent app-icon guidance (learn.microsoft.com/windows/apps/design/iconography/app-icon-design).
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "Flyout.h"
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM/GET_Y_LPARAM, the VERSION_4 tray cursor anchor macros
#include <shellapi.h>
#include <wtsapi32.h>
#include <wbemidl.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
// TrayGesture.h needs windows.h + shellapi.h, so it comes after them. Pure header, no link deps.
#include "TrayGesture.h"
// Session counters for the panel's power dashboard card. Pure header, no link deps.
#include "SessionStats.h"
// Full-screen guard decision logic. Pure header, no link deps, kept separate so --self-test
// can pin the maximised-window case that stopped this machine dimming at all.
#include "ScreenGuard.h"
// DDC/CI brightness for external monitors. The WMI path only ever drives a laptop's internal
// panel; on a desktop it answers WBEM_E_NOT_SUPPORTED and previously nothing dimmed at all.
// dxva2 is loaded by name inside the header, so this adds no link dependency.
#include "DdcBrightness.h"
#include "DisplayPower.h"
// Audio-playback fallback for the media protection. SMTC (MediaPlaying) only sees players that
// opt into publishing a session; a Chromium browser with media keys disabled publishes none, so
// the app dimmed over a playing video. WASAPI sessions are not opt-in. See the header.
#include "AudioPlayback.h"
// Software backdrop remains a compatibility fallback. Native composition is preferred.
#include "Acrylic.h"
#include "NativeBackdrop.h"

template<class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { reset(); }
    void reset() { if(p) { p->Release(); p=nullptr; } }
    T** out() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p!=nullptr; }
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
};
struct Bstr {
    BSTR p;
    explicit Bstr(const wchar_t* s):p(SysAllocString(s)){}
    ~Bstr(){SysFreeString(p);}
    operator BSTR() const{return p;}
};
// Only the read-only prefix of each COM interface is needed.
struct MediaPlayback : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Controls(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Status(INT32*)=0;
};
struct MediaSession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE AppId(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE Properties(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Timeline(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Playback(MediaPlayback**)=0;
};
struct MediaSessions : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32,MediaSession**)=0;
    virtual HRESULT STDMETHODCALLTYPE Size(UINT32*)=0;
};
struct MediaManager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Current(MediaSession**)=0;
    virtual HRESULT STDMETHODCALLTYPE Sessions(MediaSessions**)=0;
};
struct ManagerOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(IUnknown*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetCompleted(IUnknown**)=0;
    virtual HRESULT STDMETHODCALLTYPE Result(MediaManager**)=0;
};
struct ManagerFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Request(ManagerOperation**)=0;
};
struct AsyncInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Id(UINT32*)=0;
    virtual HRESULT STDMETHODCALLTYPE Status(INT32*)=0;
    virtual HRESULT STDMETHODCALLTYPE Error(HRESULT*)=0;
    virtual HRESULT STDMETHODCALLTYPE Cancel()=0;
    virtual HRESULT STDMETHODCALLTYPE Close()=0;
};
static const GUID kManagerFactory={0x2050c4ee,0x11a0,0x57de,{0xae,0xd7,0xc9,0x7c,0x70,0x33,0x82,0x45}};
static const GUID kAsyncInfo={0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const wchar_t* kClass=L"IdleDimmerNativeWindow";
static const wchar_t* kRunKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName=L"Idle Dimmer";
static const UINT kTray=WM_APP+1, kRestore=WM_APP+2, kQuit=WM_APP+3, kEnable=WM_APP+4, kReport=WM_APP+5, kShowPanel=WM_APP+6;
static const UINT kCheckTimer=1, kTestTimer=2, kPanelTimer=3, kPanelMotionTimer=4, kOffTimer=5, kCaptureWatchTimer=6;
// Posted by the low-level mouse hook when a click lands outside the panel. The hook itself
// only measures a rect and posts - it must never block, or Windows silently drops the hook.
static const UINT kPanelDismiss=WM_APP+7;
// --panel-shot: capture the panel's own rect to panel-shot.bmp (audit aid, see PanelProc).
static const UINT kPanelShot=WM_APP+12;
static const UINT kDisplayOffApply=WM_APP+14, kDisplayOffDelayTimer=7;
// --acrylic-pause: freeze the blur and clear the capture exclusion so the panel can be screenshotted.
static const UINT kAcrylicPause=WM_APP+13;
static const wchar_t* kPanelClass=L"IdleDimmerPanel";
// Command ids. The classic context menu is gone; the panel calls the same handlers directly
// from ActivatePanelTarget. Two ids survive: kDimTest because --test-brightness posts it,
// and kIconFirst because the panel's icon picker is still a native popup that reports its
// choice as a command. Both keep their original numeric values so the sparse id space does
// not need to be re-reasoned about.
static const UINT kDimTest=104, kIconFirst=120;
// Tray right-click menu command ids.
// The idle thresholds the removed context menu offered. No menu code reads this any more;
// it is kept because FlyoutLayoutSelfTest() asserts each of them is still reachable from the
// panel's slider, which is the contract that replaced the menu's "Dim after idle time".
static const DWORD kIdlePresets[6]={1,2,5,10,15,30};
// Tray icon designs; count must stay in sync with $designs in icons\build-icons.ps1 and the SVGs in icons\.
static const int kIconDesignCount=5;
static const wchar_t* kIconNames[kIconDesignCount]={L"Bulb and chevrons",L"Horizon sun, double chevrons",L"Horizon sun and arrow",L"Bulb and single chevron",L"Sunset timer (live)"};
// Design 5 is the only animated one: its six 'dN' frames track the idle countdown, so the
// tray reports how close the next dim is without being opened. Everything else is static.
static const int kDescentDesign=5;
static const int kDescentSteps=6;
static const wchar_t* kDescentNames[kDescentSteps]={L"d0",L"d1",L"d2",L"d3",L"d4",L"d5"};
static const wchar_t* kDescentPreview=L"d2";   // representative frame for the design picker
static HICON LoadStateIcon(int design,bool light,int px,const wchar_t* state);
static int TrayIconSize();
static int DescentBucket(DWORD idle,DWORD threshold);
static DWORD idleMs=120000;
static int iconDesign=4;
static bool taskbarLight=false;
static DWORD ClampIdleMinutes(long minutes){return minutes<1?1:(minutes>60?60:(DWORD)minutes);}
static int ClampIconDesign(long design){return design<1?1:(design>kIconDesignCount?kIconDesignCount:(int)design);}
// 0 = never switch the displays off. Up to an hour, matching the idle slider's range.
static DWORD ClampOffMinutes(long minutes){return minutes<0?0:(minutes>60?60:(DWORD)minutes);}
static HWND window;
static NOTIFYICONDATAW tray={};
static HICON iconOn,iconOff,iconDim;
static UINT taskbarCreated;
static bool enabled=true,protectMedia=true,protectFullscreen=true,locked=false,dimmed=false;
// Audio-based protection ("Sound"): pause dimming when any application is rendering audio, even if
// it publishes no Windows media session. Separate from protectMedia because the two signals answer
// different questions and fail independently (IMPLEMENTATION.md §7.2).
static bool protectSound=true;
// Live acrylic on the panel. On by default because the machine's native materials all degrade to a
// flat tint (Acrylic.h); off gives the plain #2C2C2C plate and nothing is captured at all.
static bool acrylicEnabled=true;
// Live refresh by default: a snapshot froze whatever was animating behind the panel (a Windows
// flyout caught mid-dismiss stays in the background), which reads as a bug. Live needs the capture
// exclusion, so --acrylic-pause freezes + drops it when a screenshot is wanted.
static bool acrylicLiveEnabled=false;
// One display-off session owns its settle timer, input fence and idle-scheduler exclusion.
static displaypower::Session displayOff;
static DWORD offMinutes=0;
static bool offInsteadOfDim=false;
// Process start supplies the power card's session uptime denominator.
static const ULONGLONG processStartTick=GetTickCount64();
static bool inputRegistered=false,restorePending=false,testing=false,closing=false;
static ULONGLONG graceUntil=0;
static unsigned checks=0;
static bool testExit=false;
static wchar_t folder[MAX_PATH],exe[MAX_PATH],ini[MAX_PATH],stateFile[MAX_PATH],statsFile[MAX_PATH],reportFile[MAX_PATH],logFile[MAX_PATH],ddcFile[MAX_PATH];
static wchar_t status[128]=L"Starting";
static Ptr<MediaManager> media;
static Ptr<ManagerOperation> mediaOperation;
static HRESULT mediaError=S_OK;
// The audio fallback's own error and counts, kept separate from the media session ones because
// the two signals fail independently and --diagnose has to say which one answered.
static HRESULT audioError=S_OK;
static int audioActiveSessions=0,audioTotalSessions=0;
struct Panel { wchar_t path[1024]; BYTE brightness; };
struct Saved { DWORD magic; DWORD count; Panel panels[4]; };
static Saved saved={};
// DDC/CI recovery state, kept in its own file with its own magic. The two mechanisms have
// different identities (a WMI instance path vs. a monitor description), different limits, and
// they fail independently, so one struct holding both would let a WMI-shaped validation rule
// reject perfectly good DDC state. 'DDC1'.
struct DdcPanel { wchar_t desc[ddc::kMaxDesc]; DWORD brightness; DWORD min; DWORD max; };
struct DdcSaved { DWORD magic; DWORD count; DdcPanel monitors[ddc::kMaxMonitors]; };
static const DWORD kDdcMagic=0x31434444;
static DdcSaved ddcSaved={};
// Durable power-dashboard counters. Survive process restarts so "Display hours saved" (and the
// rest of the card) do not reset to zero every launch. Process ticks are never persisted: they
// are meaningless across restarts, so only the aggregates and a wall-clock "last dim" time are.
struct PersistedStats { DWORD magic; DWORD version; unsigned long wakeCount; unsigned long dimCount; unsigned long restoreCount; unsigned long long dimmedMs; unsigned long long lastDimWallMs; };
static unsigned long long persistedLastDimWallMs=0;
// Lifetime dimmed time that was already on disk when this process started. The power card's
// bar compares session-only dimmed time with process uptime, so both sides must be on the same
// clock; without this base a long-lived stats.bin would paint a bar past 100%.
static unsigned long long sessionDimmedBaseMs=0;
static ULONGLONG lastStatsTick=0;
static Saved testBefore={};
static DdcSaved testBeforeDdc={};
static bool manualDim=false;
static ULONGLONG manualDimIgnoreUntil=0ULL;
// Click-away dismissal. Installed only while the panel is visible (see ShowPanel/HidePanel).
static HHOOK panelHook=nullptr;
static RECT panelBounds={};   // actual panel rect in screen coords, updated while entering
static Rect panelTarget={};
static ULONGLONG panelEntranceStart=0;
static int panelEntranceDpi=96;
static bool panelOpening=false;
static bool panelEntering=false;
static bool panelExiting=false;
static int panelExitFromOffset=0;

static HWND panelWindow=nullptr;static bool PanelHasCorners=false;
// How many panel paints used a live acrylic frame. Reported by --status: the worker producing
// frames proves the pipeline runs, this proves the painter consumed them.
static unsigned acrylicPaints=0;static void WriteRuntimeReport() {
    FILE* f=_wfopen(reportFile,L"w, ccs=UTF-8");
    if(f){
        const wchar_t* acrylicText;
        if(!acrylicEnabled)acrylicText=L"off (flat plate)";
        else if(!panelWindow||!IsWindowVisible(panelWindow))acrylicText=L"inactive (panel hidden)";
        else if(nativebackdrop::Running())acrylicText=L"native Windows host backdrop";
        else if(acrylic::Running())acrylicText=L"live software fallback";
        else if(acrylic::Paused())acrylicText=L"frozen for screenshots";
        else if(acrylic::Frames()>0)acrylicText=L"snapshot at open (no refresh; screenshots work)";
        else acrylicText=L"snapshot pending";
        fwprintf(f,L"Status: %ls\nEnabled: %d\nDimmed: %d\nDisplays off: %d\nIdle threshold: %u minutes\nIdle checks: %u\nInput notifications registered: %d\nAcrylic: %ls; frames %d; painted %u\n",status,enabled,dimmed,displayOff.OffRequested(),idleMs/60000,checks,inputRegistered,acrylicText,acrylic::Frames(),acrylicPaints);
        // Panel observability. There is no interactive preview in this environment, so the
        // panel's existence, visibility and which DWM tier it got are reported here rather
        // than assumed from the fact that the code compiled.
        fwprintf(f,L"Display-off session: %u; phase %u\n",displayOff.generation,(unsigned)displayOff.phase);
        fwprintf(f,L"Panel window: %ls\nPanel visible: %ls\nPanel rect: %ld,%ld %ldx%ld\nBackdrop: %ls\nRounded corners: %ls\n",
                 panelWindow?L"created":L"not created",
                 (panelWindow&&IsWindowVisible(panelWindow))?L"yes":L"no",
                 panelBounds.left,panelBounds.top,panelBounds.right-panelBounds.left,panelBounds.bottom-panelBounds.top,
                 (!panelWindow||!IsWindowVisible(panelWindow))?L"inactive":
                 (nativebackdrop::Running()?L"native acrylic":(acrylic::Frames()>0?L"software acrylic fallback":L"opaque fallback")),
                 PanelHasCorners?L"yes":L"no");
        fwprintf(f,L"Native composition: %ls; last stage %ls; HRESULT 0x%08lX; presentations %u\nNative worker alive: %ls\n",
                 nativebackdrop::Running()?L"running":L"stopped",nativebackdrop::Stage(),
                 (unsigned long)nativebackdrop::LastError(),nativebackdrop::Frames(),nativebackdrop::WorkerAlive()?L"yes":L"no");
        fwprintf(f,L"Native animation: %ls; stage %ls; HRESULT 0x%08lX; completed %u\n",
                 nativebackdrop::AnimationPhase(),nativebackdrop::AnimationStage(),(unsigned long)nativebackdrop::AnimationError(),
                 nativebackdrop::CompletedAnimations());
        fwprintf(f,L"Native first frame ready: %ls; window cloaked: %ls\n",
                 nativebackdrop::FirstFrameReady()?L"yes":L"no",nativebackdrop::Cloaked()?L"yes":L"no");
        if(nativebackdrop::Running())fwprintf(f,L"Native compositor output: use Snipping Tool; --panel-shot renders software fallback only\n");
        fclose(f);
    }
}

static void Log(const wchar_t* message) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if(GetFileAttributesExW(logFile,GetFileExInfoStandard,&a) && a.nFileSizeLow>65536) DeleteFileW(logFile);
    FILE* f=_wfopen(logFile,L"a, ccs=UTF-8");
    if(f){SYSTEMTIME t;GetLocalTime(&t);fwprintf(f,L"%04u-%02u-%02u %02u:%02u:%02u %ls\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,message);fclose(f);}
}
static void ErrorStatus(const wchar_t* message,HRESULT hr) {
    swprintf(status,128,L"%ls (0x%08lX)",message,(unsigned long)hr);
    Log(status);
}
static HRESULT ConnectWmi(Ptr<IWbemServices>& svc) {
    Ptr<IWbemLocator> loc;
    HRESULT hr=CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)loc.out());
    if(FAILED(hr))return hr;
    Bstr ns(L"ROOT\\WMI");
    hr=loc->ConnectServer(ns,nullptr,nullptr,nullptr,0,nullptr,nullptr,svc.out());
    if(FAILED(hr))return hr;
    return CoSetProxyBlanket(svc.p,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
}
static HRESULT Query(IWbemServices* svc,const wchar_t* query,Ptr<IEnumWbemClassObject>& rows) {
    Bstr lang(L"WQL"),q(query);
    return svc->ExecQuery(lang,q,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,rows.out());
}
static HRESULT Capture(Saved& result) {
    result={};result.magic=0x53444932;
    Ptr<IWbemServices> svc; HRESULT hr=ConnectWmi(svc);if(FAILED(hr))return hr;
    Ptr<IEnumWbemClassObject> rows;
    hr=Query(svc.p,L"SELECT InstanceName, CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE",rows);if(FAILED(hr))return hr;
    for(;;){
        Ptr<IWbemClassObject> obj;ULONG count=0;
        hr=rows->Next(3000,1,obj.out(),&count);if(FAILED(hr))return hr;if(!count)break;
        if(result.count>=4)return E_BOUNDS;
        VARIANT name,brightness;VariantInit(&name);VariantInit(&brightness);
        hr=obj->Get(L"InstanceName",0,&name,nullptr,nullptr);
        if(SUCCEEDED(hr))hr=obj->Get(L"CurrentBrightness",0,&brightness,nullptr,nullptr);
        if(FAILED(hr)||name.vt!=VT_BSTR){VariantClear(&name);VariantClear(&brightness);return E_FAIL;}
        VARIANT b;VariantInit(&b);hr=VariantChangeType(&b,&brightness,0,VT_UI1);
        Panel p={};if(SUCCEEDED(hr))p.brightness=b.bVal;
        Ptr<IEnumWbemClassObject> methods;
        if(SUCCEEDED(hr))hr=Query(svc.p,L"SELECT InstanceName, __PATH FROM WmiMonitorBrightnessMethods WHERE Active=TRUE",methods);
        bool found=false;
        while(SUCCEEDED(hr)){
            Ptr<IWbemClassObject> method;ULONG n=0;hr=methods->Next(3000,1,method.out(),&n);if(FAILED(hr)||!n)break;
            VARIANT mn,mp;VariantInit(&mn);VariantInit(&mp);
            HRESULT r=method->Get(L"InstanceName",0,&mn,nullptr,nullptr);
            if(SUCCEEDED(r)&&mn.vt==VT_BSTR&&wcscmp(name.bstrVal,mn.bstrVal)==0){
                r=method->Get(L"__PATH",0,&mp,nullptr,nullptr);
                if(SUCCEEDED(r)&&mp.vt==VT_BSTR&&wcslen(mp.bstrVal)<1024){wcscpy(p.path,mp.bstrVal);found=true;}
            }
            VariantClear(&mn);VariantClear(&mp);if(found)break;
        }
        VariantClear(&b);VariantClear(&name);VariantClear(&brightness);
        if(FAILED(hr)||!found)return FAILED(hr)?hr:E_FAIL;
        result.panels[result.count++]=p;
    }
    return result.count?S_OK:HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
static HRESULT SetLevels(const Saved& s,bool restore) {
    if(!s.count)return S_OK;
    Ptr<IWbemServices> svc;HRESULT hr=ConnectWmi(svc);if(FAILED(hr))return hr;
    Ptr<IWbemClassObject> cls,inDef;Bstr cn(L"WmiMonitorBrightnessMethods"),mn(L"WmiSetBrightness");
    hr=svc->GetObject(cn,0,nullptr,cls.out(),nullptr);if(FAILED(hr))return hr;
    hr=cls->GetMethod(mn,0,inDef.out(),nullptr);if(FAILED(hr))return hr;
    HRESULT first=S_OK;
    for(DWORD i=0;i<s.count;i++){
        Ptr<IWbemClassObject> args,out;hr=inDef->SpawnInstance(0,args.out());
        VARIANT v;VariantInit(&v);v.vt=VT_I4;v.lVal=0;
        if(SUCCEEDED(hr))hr=args->Put(L"Timeout",0,&v,CIM_UINT32);
        v.vt=VT_UI1;v.bVal=restore?s.panels[i].brightness:0;
        if(SUCCEEDED(hr))hr=args->Put(L"Brightness",0,&v,CIM_UINT8);
        Bstr path(s.panels[i].path);
        if(SUCCEEDED(hr))hr=svc->ExecMethod(path,mn,0,nullptr,args.p,out.out(),nullptr);
        if(SUCCEEDED(hr)&&out){
            VARIANT ret;VariantInit(&ret);
            if(SUCCEEDED(out->Get(L"ReturnValue",0,&ret,nullptr,nullptr))&&ret.vt!=VT_NULL&&ret.vt!=VT_EMPTY){
                VARIANT n;VariantInit(&n);if(SUCCEEDED(VariantChangeType(&n,&ret,0,VT_UI4))&&n.ulVal!=0)hr=E_FAIL;VariantClear(&n);
            }VariantClear(&ret);
        }
        if(FAILED(hr)&&SUCCEEDED(first))first=hr;
    }
    return first;
}
static bool SaveBrightness() {
    HANDLE f=CreateFileW(stateFile,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;bool ok=WriteFile(f,&saved,sizeof(saved),&n,nullptr)&&n==sizeof(saved);FlushFileBuffers(f);CloseHandle(f);return ok;
}
static bool LoadBrightness() {
    HANDLE f=CreateFileW(stateFile,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
    Saved s={};DWORD n=0;bool ok=ReadFile(f,&s,sizeof(s),&n,nullptr)&&n==sizeof(s);CloseHandle(f);
    if(!ok||s.magic!=0x53444932||!s.count||s.count>4)return false;
    for(DWORD i=0;i<s.count;i++)if(s.panels[i].path[1023]!=0||s.panels[i].brightness>100||!wcsstr(s.panels[i].path,L"WmiMonitorBrightnessMethods."))return false;
    saved=s;return true;
}
// DDC/CI, the path that actually exists on a desktop with external monitors. See
// DdcBrightness.h for the measurement that forced it and for how dxva2 is resolved.
static HRESULT CaptureDdc(DdcSaved& out) {
    out={};out.magic=kDdcMagic;
    ddc::Monitor monitors[ddc::kMaxMonitors];
    int count=ddc::Enumerate(monitors,ddc::kMaxMonitors);
    for(int i=0;i<count;i++){
        wcsncpy(out.monitors[i].desc,monitors[i].desc,ddc::kMaxDesc-1);
        out.monitors[i].brightness=monitors[i].current;
        out.monitors[i].min=monitors[i].min;
        out.monitors[i].max=monitors[i].max;
    }
    out.count=(DWORD)count;
    return count?S_OK:HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
static HRESULT SetDdcLevels(const DdcSaved& s,bool restore) {
    if(!s.count)return S_OK;
    // Occurrence, not index: all three measured monitors report "Generic PnP Monitor", so the
    // second saved entry has to mean the second such monitor. See ddc::FindMonitor.
    const wchar_t* descs[ddc::kMaxMonitors]={};
    for(DWORD i=0;i<s.count;i++)descs[i]=s.monitors[i].desc;
    HRESULT first=S_OK;
    for(DWORD i=0;i<s.count;i++){
        DWORD level=restore?s.monitors[i].brightness:0;
        int occurrence=ddc::Occurrence(descs,(int)i,s.monitors[i].desc);
        if(!ddc::SetLevel(s.monitors[i].desc,occurrence,level)&&SUCCEEDED(first))first=E_FAIL;
    }
    return first;
}
static bool SaveDdcBrightness() {
    if(!ddcSaved.count)return true;
    HANDLE f=CreateFileW(ddcFile,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;bool ok=WriteFile(f,&ddcSaved,sizeof(ddcSaved),&n,nullptr)&&n==sizeof(ddcSaved);FlushFileBuffers(f);CloseHandle(f);return ok;
}
static bool LoadDdcBrightness() {
    HANDLE f=CreateFileW(ddcFile,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
    DdcSaved s={};DWORD n=0;bool ok=ReadFile(f,&s,sizeof(s),&n,nullptr)&&n==sizeof(s);CloseHandle(f);
    if(!ok||s.magic!=kDdcMagic||!s.count||s.count>ddc::kMaxMonitors)return false;
    for(DWORD i=0;i<s.count;i++)if(s.monitors[i].desc[ddc::kMaxDesc-1]!=0||s.monitors[i].brightness>100)return false;
    ddcSaved=s;return true;
}
static unsigned long long WallNowMs() {
    FILETIME ft;GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;u.LowPart=ft.dwLowDateTime;u.HighPart=ft.dwHighDateTime;
    return u.QuadPart/10000ULL;
}
// Wall-clock now minus wall-clock then, saturated at 0. NOT a wrapper for convenience.
//
// Unlike GetTickCount64, the wall clock is not monotonic: an NTP correction, a manual clock change,
// a DST boundary or a resume from sleep can all move it backwards. Subtracting two unsigned
// long longs in that case underflows to near 2^64, and the card would print something like
// "17050329600000 min" - a number large enough to look like a real measurement and mean
// nothing. A negative delta is not a very old dim, it is an unknown one, so it reports 0 and the
// card falls back to the em dash. Pure, so --self-test can pin it without touching the clock.
static unsigned long long WallDeltaMs(unsigned long long now,unsigned long long then) {
    return now>=then?now-then:0ULL;
}
static void LoadStats() {
    HANDLE f=CreateFileW(statsFile,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(f==INVALID_HANDLE_VALUE)return;
    PersistedStats p={};DWORD n=0;bool ok=ReadFile(f,&p,sizeof(p),&n,nullptr)&&n==sizeof(p);CloseHandle(f);
    if(!ok||p.magic!=0x53545453||p.version!=1)return;
    sessionstats::Counters c={};
    c.wakeCount=p.wakeCount;c.dimCount=p.dimCount;c.restoreCount=p.restoreCount;
    c.dimmedMs=p.dimmedMs;c.hasDimHistory=(p.dimCount>0);
    sessionstats::Seed(c);
    persistedLastDimWallMs=p.lastDimWallMs;
    sessionDimmedBaseMs=p.dimmedMs;
}
static void SaveStats() {
    sessionstats::Counters c=sessionstats::Get();
    PersistedStats p={};
    p.magic=0x53545453;p.version=1;
    p.wakeCount=c.wakeCount;p.dimCount=c.dimCount;p.restoreCount=c.restoreCount;
    p.dimmedMs=c.dimmedMs;p.lastDimWallMs=persistedLastDimWallMs;
    HANDLE f=CreateFileW(statsFile,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    DWORD wrote=0;WriteFile(f,&p,sizeof(p),&wrote,nullptr);FlushFileBuffers(f);CloseHandle(f);
}
static bool InputEvents(bool on) {
    RAWINPUTDEVICE devices[4]={{1,2,0,nullptr},{1,6,0,nullptr},{13,4,0,nullptr},{13,5,0,nullptr}};
    for(auto& d:devices){d.dwFlags=on?RIDEV_INPUTSINK:RIDEV_REMOVE;d.hwndTarget=on?window:nullptr;}
    bool ok=RegisterRawInputDevices(devices,4,sizeof(RAWINPUTDEVICE))!=FALSE;
    if(ok)inputRegistered=on;return ok;
}
// Which descent frame the countdown is on. Pure, so --self-test can exercise it without a
// window: the whole point of extracting it is that the icon's behaviour is then provable.
static int DescentBucket(DWORD idle,DWORD threshold) {
    if(!threshold)return 0;
    int step=(int)(((LONGLONG)idle*kDescentSteps)/threshold);
    return step<0?0:(step>=kDescentSteps?kDescentSteps-1:step);
}
// The live descent frame is cached rather than reloaded on every tick: one ico decode is
// far more expensive than the NIM_MODIFY that shows it. Invalidate on theme change only.
static HICON iconLive=nullptr;
static int iconLiveStep=-1;
static bool iconLiveValid=false;
static HICON EnsureDescentIcon(int step) {
    if(iconLiveValid&&iconLiveStep==step)return iconLive;
    // taskbarLight is the value LoadStateIcon was last called with, and LoadIcons keeps it
    // current, so reusing it keeps the live frame on the same theme variant as the others.
    HICON next=LoadStateIcon(kDescentDesign,taskbarLight,TrayIconSize(),kDescentNames[step]);
    if(!next)return nullptr;                       // artwork missing: caller falls back
    if(iconLive)DestroyIcon(iconLive);
    iconLive=next;iconLiveStep=step;iconLiveValid=true;
    return iconLive;
}
static void DropDescentIcon() {
    if(iconLive)DestroyIcon(iconLive);
    iconLive=nullptr;iconLiveStep=-1;iconLiveValid=false;
}
static void UpdateTray() {
    HICON show=nullptr;
    if(!enabled)show=iconOff;
    else if(dimmed)show=iconDim;
    else if(iconDesign==kDescentDesign){
        // Windows' own last-input counter, same source the dim decision uses, so the icon
        // cannot disagree with what Check() is about to do.
        LASTINPUTINFO input={sizeof(input),0};
        DWORD idle=GetLastInputInfo(&input)?GetTickCount()-input.dwTime:0;
        show=EnsureDescentIcon(DescentBucket(idle,idleMs));
        if(!show)show=iconOn;                       // sunset artwork absent
    }else show=iconOn;
    tray.hIcon=show;
    swprintf(tray.szTip,128,L"Idle Dimmer - %.100ls",status);
    tray.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE;Shell_NotifyIconW(NIM_MODIFY,&tray);
}
// ---------------------------------------------------------------------------
// Switching the displays off
// ---------------------------------------------------------------------------
// Use one documented DefWindowProcW display-power request on our own window, rather than
// a synchronous broadcast through every other app. The manual button settles before off;
// no hardware brightness query/write, polling or repeated power request belongs to that path.
static void ScheduleNext();
static bool Restore(bool requestPowerOn=true);
static bool SetKeepAwake(bool includeDisplay) {
    return SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|
                                  (includeDisplay?ES_DISPLAY_REQUIRED:0))!=0;
}
static void AbortDisplayOff(const wchar_t* message,HRESULT hr) {
    displayOff.Cancel();KillTimer(window,kDisplayOffDelayTimer);
    if(!dimmed)InputEvents(false);
    ErrorStatus(message,hr);UpdateTray();ScheduleNext();
}
static void ApplyDisplayOff() {
    const ULONGLONG now=GetTickCount64();
    if(displayOff.phase!=displaypower::settling||now<displayOff.due)return;
    LASTINPUTINFO input={sizeof(input),0};
    if(!GetLastInputInfo(&input)){
        AbortDisplayOff(L"Display-off input check failed",HRESULT_FROM_WIN32(GetLastError()));return;
    }
    // Automatic off was posted from a timer decision; input may have arrived before this
    // message. A manual countdown deliberately allows settling input, an idle request does not.
    if(displayOff.CancelForInput((DWORD)(GetTickCount()-input.dwTime),idleMs,manualDim)){
        Restore();graceUntil=GetTickCount64()+idleMs;ScheduleNext();return;
    }
    if(!SetKeepAwake(false)){
        AbortDisplayOff(L"Display-off power request failed",E_FAIL);return;
    }
    if(!displayOff.Commit(now,input.dwTime))return;
    KillTimer(window,kDisplayOffDelayTimer);
    if(!dimmed){
        dimmed=true;lastStatsTick=now;persistedLastDimWallMs=WallNowMs();
        sessionstats::NoteDim(now);SaveStats();
    }
    const bool brightnessChanged=saved.count>0||ddcSaved.count>0;
    wcscpy(status,brightnessChanged?L"Displays off; input restores brightness":L"Displays off; input wakes them");
    UpdateTray();
    // Do this last: no tray refresh or brightness traffic after asking Windows to turn off.
    DefWindowProcW(window,WM_SYSCOMMAND,SC_MONITORPOWER,2);
}
static void OffDisplays(bool fromButton=false) {
    if(displayOff.Active())return;
    if(!inputRegistered&&!InputEvents(true)){
        ErrorStatus(L"Input notifications unavailable",HRESULT_FROM_WIN32(GetLastError()));
        UpdateTray();return;
    }
    if(!displayOff.Begin(GetTickCount64(),fromButton,inputRegistered))return;
    restorePending=false;
    KillTimer(window,kCheckTimer);KillTimer(window,kOffTimer);
    if(fromButton){
        if(!SetTimer(window,kDisplayOffDelayTimer,(UINT)displaypower::kManualSettleMs,nullptr)){
            AbortDisplayOff(L"Display-off timer unavailable",HRESULT_FROM_WIN32(GetLastError()));return;
        }
        wcscpy(status,L"Displays off in 5 seconds");UpdateTray();
    }else if(!PostMessageW(window,kDisplayOffApply,displayOff.generation,0)){
        AbortDisplayOff(L"Display-off message unavailable",HRESULT_FROM_WIN32(GetLastError()));
    }
}
static void WakeDisplays(bool requestPowerOn=true) {
    if(!displayOff.Active())return;
    const bool wasOff=displayOff.OffRequested();
    displayOff.Cancel();KillTimer(window,kDisplayOffDelayTimer);restorePending=false;
    if(requestPowerOn){
        if(enabled&&!locked)SetKeepAwake(true);
        else SetThreadExecutionState(ES_CONTINUOUS);
    }
    if(!dimmed){InputEvents(false);wcscpy(status,L"Enabled; waiting for idle");UpdateTray();}
    if(requestPowerOn&&wasOff){
        DefWindowProcW(window,WM_SYSCOMMAND,SC_MONITORPOWER,-1);
    }
}
static void StartOffInsteadOfDim() { OffDisplays(); }
static bool Restore(bool requestPowerOn) {
    // Input/quit restore may wake displays. Lock and suspend cancel pending off work without
    // issuing a competing power-on command. Brightness captured before a dim is still restored.
    KillTimer(window,kOffTimer);
    WakeDisplays(requestPowerOn);
    const bool anySaved=saved.count>0||ddcSaved.count>0;
    // Nothing captured and nothing dark: the ordinary armed-and-awake case, nothing to do.
    // `dimmed` is part of the test because "off instead of dim" sets it without capturing a level
    // - that state still has to be cleared, or the scheduler would sit in its dimmed cadence and
    // the app would behave as if a dim were outstanding forever.
    if(!anySaved&&!dimmed)return true;
    // Both mechanisms are restored even when only one of them dimmed: with a laptop panel and
    // external monitors attached, one dim may have touched both, and skipping the empty one
    // costs nothing (SetLevels/SetDdcLevels return S_OK with zero entries).
    if(anySaved){
        HRESULT hr=SetLevels(saved,true);
        HRESULT dhr=SetDdcLevels(ddcSaved,true);
        if(FAILED(hr)||FAILED(dhr)){ErrorStatus(L"Restore failed; retrying",FAILED(hr)?hr:dhr);UpdateTray();return false;}
    }
    // Close out the final dimmed segment before clearing `dimmed`. The scheduler's dimmed
    // cadence is two minutes (kCheckDimmedMs), so an ordinary input restore arrives long before
    // the next check. Without this line the last - usually only - segment of every short dim was
    // never counted, which kept the card's bar at zero and undercounted "Display hours saved":
    // 79 recorded dims had accumulated 7.8 h, i.e. only the long ones.
    if(dimmed&&lastStatsTick){sessionstats::AccumulateDimmed(GetTickCount64()-lastStatsTick);lastStatsTick=0;}
    saved={};ddcSaved={};dimmed=false;manualDim=false;manualDimIgnoreUntil=0ULL;DeleteFileW(stateFile);DeleteFileW(ddcFile);InputEvents(false);restorePending=false;
    // Counted on the success path only, and before the return: a restore that failed and is being
    // retried has not restored anything yet. Placed after `return true` it would be dead code.
    sessionstats::NoteRestore(GetTickCount64());
    wcscpy(status,L"Enabled; waiting for idle");Log(anySaved?L"Previous brightness restored":L"Displays woken; brightness unchanged");UpdateTray();SaveStats();return true;
}
static bool Dim() {
    // Two independent mechanisms, captured together so the restore is symmetric:
    //   WMI      - a laptop's internal panel (WmiSetBrightness)
    //   DDC/CI   - external monitors (dxva2 SetMonitorBrightness)
    // Either one working is enough to dim. Before this, a desktop with no internal panel failed
    // here with "Brightness unavailable" and nothing ever dimmed: MEASURED 2026-10-31, WMI
    // answered 0x8004100C (WBEM_E_NOT_SUPPORTED) with 0 internal panels while all three external
    // monitors answered DDC.
    HRESULT hr=Capture(saved);
    HRESULT dhr=CaptureDdc(ddcSaved);
    if(FAILED(hr)&&FAILED(dhr)){
        saved={};ddcSaved={};
        ErrorStatus(L"Brightness unavailable",FAILED(hr)?hr:dhr);
        UpdateTray();return false;
    }
    if((saved.count&&!SaveBrightness())||(ddcSaved.count&&!SaveDdcBrightness())){
        saved={};ddcSaved={};
        ErrorStatus(L"Cannot save brightness",HRESULT_FROM_WIN32(GetLastError()));return false;
    }
    if(!InputEvents(true)){Restore();ErrorStatus(L"Input notifications unavailable",HRESULT_FROM_WIN32(GetLastError()));return false;}
    // Recheck idle state after the brightness queries to avoid dimming over fresh user input.
    LASTINPUTINFO input={sizeof(input),0};
    if(!testing&&!manualDim&&(!GetLastInputInfo(&input)||(DWORD)(GetTickCount()-input.dwTime)<idleMs)){Restore();return false;}
    hr=SetLevels(saved,false);
    dhr=SetDdcLevels(ddcSaved,false);
    if(FAILED(hr)&&FAILED(dhr)){Restore();ErrorStatus(L"Dimming failed",FAILED(hr)?hr:dhr);UpdateTray();return false;}
    const ULONGLONG dimTick=GetTickCount64();
    sessionstats::NoteDim(dimTick);
    // The dimmed interval starts here. Check() refreshes this on every dimmed tick, but a dim
    // started from the panel's footer button or --test-brightness never passes through Check(),
    // so without this the first restore would charge all the awake time since the last check to
    // the dim. Restore() closes the interval with now - lastStatsTick.
    lastStatsTick=dimTick;
    persistedLastDimWallMs=WallNowMs();
    dimmed=true;wcscpy(status,L"Dimmed; input restores brightness");Log(L"Dimmed to minimum brightness");UpdateTray();SaveStats();
    // Second stage: after the configured delay, stop emitting light entirely. Armed here rather
    // than in Check() so a dim started from the panel button or --test-brightness behaves the
    // same as a scheduled one; Restore() kills it. 0 = keep the old dim-only behaviour.
    if(offMinutes)SetTimer(window,kOffTimer,offMinutes*60000,nullptr);
    return true;
}
static HRESULT InitMedia() {
    if(media||mediaOperation){return S_OK;}
    HSTRING name=nullptr;const wchar_t* cls=L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    HRESULT hr=WindowsCreateString(cls,(UINT32)wcslen(cls),&name);
    if(FAILED(hr)){return hr;}
    Ptr<ManagerFactory> factory;hr=RoGetActivationFactory(name,kManagerFactory,(void**)factory.out());WindowsDeleteString(name);
    if(SUCCEEDED(hr))hr=factory->Request(mediaOperation.out());
    return hr;
}
// -1 means unknown: protect the user from unwanted dimming when detection fails.
static int MediaPlaying() {
    mediaError=InitMedia();
    if(FAILED(mediaError)){return -1;}
    if(!media){
        // IAsyncInfo::Status: 0 Started, 1 Completed, 2 Canceled, 3 Error.
        Ptr<AsyncInfo> info;mediaError=mediaOperation->QueryInterface(kAsyncInfo,(void**)info.out());
        if(FAILED(mediaError)){return -1;}
        INT32 s=0;
        // This function must never return while leaving the request outstanding: releasing an
        // operation that never completed is what faulted in MediaControl.dll across a suspend
        // (IMPLEMENTATION.md s7.1). Wait a bound (~300 ms in 20 ms steps, the --diagnose retry
        // pattern), then cancel it, then release it.
        for(int i=0;i<15;i++){
            mediaError=info->Status(&s);
            if(FAILED(mediaError)||s!=0)break;   // Started (0) means keep waiting; anything else is an answer
            Sleep(20);
        }
        if(FAILED(mediaError)){
            // The status call itself failed, so the request can no longer be collected. Cancel it
            // rather than release it outright - cancel-then-release is the order that is safe.
            info->Cancel();
            mediaOperation.reset();
            return -1;
        }
        if(s==0){
            info->Cancel();
            mediaOperation.reset();
            // Report the timeout rather than S_OK: it is what happened, and it stops the
            // --diagnose retry loop from rebuilding and re-timing-out a request 99 more times.
            mediaError=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return -1;
        }
        if(s!=1){
            info->Error(&mediaError);
            mediaOperation.reset();
            return -1;
        }
        mediaError=mediaOperation->Result(media.out());
        mediaOperation.reset();
        if(FAILED(mediaError)){return -1;}
    }
    Ptr<MediaSessions> sessions;mediaError=media->Sessions(sessions.out());
    if(FAILED(mediaError)){
        media.reset();
        return -1;
    }
    UINT32 count=0;mediaError=sessions->Size(&count);
    if(FAILED(mediaError)){return -1;}
    bool unknown=false;
    for(UINT32 i=0;i<count;i++){
        Ptr<MediaSession> session;Ptr<MediaPlayback> playback;INT32 state=0;
        HRESULT hr=sessions->GetAt(i,session.out());
        if(SUCCEEDED(hr))hr=session->Playback(playback.out());
        if(SUCCEEDED(hr))hr=playback->Status(&state);
        if(FAILED(hr)){unknown=true;mediaError=hr;continue;}
        if(state==4||state==5){return 1;} // Playing or Paused.
    }
    return unknown?-1:0;
}
// What the media protection actually consumes.
//
// SMTC is asked first and its answer is final when it is positive: it knows Paused and it names
// the player. Only when it reports nothing does the WASAPI fallback run, because a player that
// publishes no session is invisible to SMTC and would otherwise be dimmed over.
//
// Returns 1 = a media session is Playing/Paused, 2 = audio is rendering on the default endpoint,
// 0 = nothing is in use, -1 = neither signal could be read (unknown must pause dimming).
static int MediaInUse() {
    // Two switches, two signals. "Media" is the Windows session list (it distinguishes Paused and
    // names the player); "Sound" is the WASAPI fallback that closes the hole a browser with media
    // keys disabled leaves (§7.2). They are asked separately because they can now be switched off
    // separately: turning Sound off must not make a working media session invisible, and vice
    // versa, so a positive answer from either one still pauses dimming.
    int playing=protectMedia?MediaPlaying():0;
    if(playing!=0)return playing;            // playing, or the session query itself failed
    audioActiveSessions=0;audioTotalSessions=0;
    if(!protectSound){audioError=S_OK;return 0;}
    int state=audio::AnyActive(&audioError,&audioActiveSessions,&audioTotalSessions);
    return state==1?2:state;
}
static bool Fullscreen() {
    // The shell state is kept. All three values are documented full-screen or presentation
    // states, so dropping them would regress titles that report themselves correctly. It is NOT
    // the cause of the bug fixed below: on the machine where that bug was found this call
    // returned QUNS_ACCEPTS_NOTIFICATIONS, the normal state.
    QUERY_USER_NOTIFICATION_STATE s;
    if(SUCCEEDED(SHQueryUserNotificationState(&s))&&(s==QUNS_BUSY||s==QUNS_RUNNING_D3D_FULL_SCREEN||s==QUNS_PRESENTATION_MODE))return true;
    HWND fg=GetForegroundWindow();
    guard::FgInfo f;
    f.resolved=fg!=nullptr;
    if(!f.resolved)return false;
    // The shell window, the desktop and Flip3D all span the monitor and are not applications.
    // PowerToys special-cases the first three; without it each one is a false positive.
    if(fg==GetShellWindow()||fg==GetDesktopWindow()){f.shellSurface=true;return false;}
    wchar_t cls[64]={};
    if(GetClassNameW(fg,cls,64)){
        if(wcscmp(cls,L"Progman")==0||wcscmp(cls,L"WorkerW")==0||wcscmp(cls,L"Flip3D")==0){
            f.shellSurface=true;return false;
        }
        // MEASURED 2026-10-17: the taskbar / Start / flyout host is a shell XAML island whose
        // foreground window carries style 0x94000000 - WS_POPUP|WS_VISIBLE, with no caption, no
        // system menu, no sizing border and no WS_MAXIMIZE - and a rect covering the whole monitor.
        // Every style exclusion passes it, so it read as a full-screen game and the guard latched
        // on again. It is shell chrome, not an application, and has no class name that could be
        // enumerated in advance, so it is matched by the window's owning process instead: the
        // shell is explorer.exe. Comparing the window's PID is cheaper and more durable than
        // chasing class names the shell renames between builds.
        // GetShellWindow()'s own PID is the shell's, i.e. explorer.exe. Deriving it rather than
        // hardcoding "explorer.exe" keeps this correct if the shell is ever hosted elsewhere.
        DWORD pid=0,dw=0;
        GetWindowThreadProcessId(fg,&pid);
        GetWindowThreadProcessId(GetShellWindow(),&dw);
        if(pid&&pid==dw){f.shellSurface=true;return false;}
    }
    f.style=(unsigned long)GetWindowLongW(fg,GWL_STYLE);
    MONITORINFO m={sizeof(m)};
    if(!GetWindowRect(fg,&f.rect)||!GetMonitorInfoW(MonitorFromWindow(fg,MONITOR_DEFAULTTONEAREST),&m))return false;
    f.monitor=m.rcMonitor;
    return guard::LooksFullScreen(f);
}
static bool ShouldDim(bool on,bool isLocked,DWORD idle,ULONGLONG now,ULONGLONG grace,DWORD threshold,bool mediaBlocked,bool fullscreenBlocked) {
    return on&&!isLocked&&idle>=threshold&&now>=grace&&!mediaBlocked&&!fullscreenBlocked;
}
// Scheduler bounds. Declared before ScheduleWaitMs, which references them.
static const DWORD kCheckMaxMs=15*60*1000;   // 15 minute backstop
static const DWORD kCheckMinMs=250;          // never busy-spin
// Awake-and-idle cadence. See ScheduleWaitMs for why a ready-to-dim decision must not sleep for
// the backstop instead.
static const DWORD kCheckReadyMs=30*1000;
// Already dimmed: nothing to decide until input arrives, and the raw-input sink catches that
// without polling. Long enough that the timer is free, short enough that a missed restore is not
// left standing for minutes if the sink ever fails.
static const DWORD kCheckDimmedMs=2*60*1000;
// Pure twin of ScheduleNext's wait computation, extracted so --self-test can pin the sleep that
// actually happened in the field: a declined dim armed 15 minutes and the app never woke again.
static DWORD ScheduleWaitMs(DWORD idle,DWORD threshold,bool graceOutstanding,DWORD graceAgeMs,bool dimmed) {
    // Already dimmed. The only thing a wake can accomplish is noticing input and restoring, and the
    // raw-input sink registered while dimmed already does that at the OS level, without any timer.
    // So this state has no reason to wake often at all - and the first version of this function
    // returned kCheckReadyMs here too, which meant 250 ms gaps (measured: median 265 ms, ~144
    // checks in two minutes). That is precisely the idle CPU this app exists to avoid, caused by
    // the very fix for the never-dims bug.
    if(dimmed)return kCheckDimmedMs;
    DWORD wait=kCheckMaxMs;
    // A dim is possible once BOTH horizons are behind us. While either is still counting down, wake
    // for whichever expires first - that is the only moment a decision can change.
    if(idle<threshold){const DWORD remain=threshold-idle;if(remain<wait)wait=remain;}
    if(graceOutstanding){
        // graceAgeMs is how long ago the grace period began; it ends a full threshold from then.
        const DWORD graceLeft=graceAgeMs>=threshold?kCheckMinMs:threshold-graceAgeMs;
        if(graceLeft<wait)wait=graceLeft;
    }else if(idle>=threshold){
        // Ready to dim right now. Sleeping for the backstop here is the field defect: the decision
        // that declined to dim armed 15 minutes and the app never woke again.
        wait=kCheckReadyMs;
    }
    if(wait<kCheckMinMs)wait=kCheckMinMs;
    if(wait>kCheckMaxMs)wait=kCheckMaxMs;
    return wait;
}
// Choose when the next check is actually needed, instead of waking on a fixed 30 s period.
//
// The previous design woke twice a minute and, on every wake, asked two expensive questions -
// is media playing (a WinRT round-trip) and is anything full-screen - before it even looked at
// the clock. Both answers only matter at the instant we are about to dim. In between, the
// process is asleep with nothing to decide.
//
// So: sleep until the idle countdown can next reach the threshold, capped by a long backstop so
// a coalesced or missed timer cannot strand the app with no timer armed at all.
//
// The cap is a stranding guard, not a normal operating point. If a decision declines to dim while
// already idle-and-ready, sleeping for the cap means the app does not reconsider until a quarter of
// an hour has passed - and nothing else re-arms it, so it never dims. That is not a theoretical
// risk: it is what happened, visible as checks=1 frozen in the field log across a three minute
// watch with the threshold long past. `ScheduleWaitMs` therefore wakes at kCheckReadyMs in that
// state, and the reasoning is recorded where the test can pin it.
//
// Media detection does not regress. If a video starts after a check, the next wake is still a
// decision point: it sees the video and declines to dim. What goes stale between checks is the
// status line, and only while nothing is reading it - with the panel open the 1 s tick re-asks.
static void ScheduleNext() {
    if(!enabled||locked||displayOff.Active()){KillTimer(window,kCheckTimer);return;}
    LASTINPUTINFO probe={sizeof(probe),0};
    if(!GetLastInputInfo(&probe)){SetCoalescableTimer(window,kCheckTimer,kCheckMaxMs,nullptr,1000);return;}
    const DWORD idle=GetTickCount()-probe.dwTime;
    const ULONGLONG nowTick=GetTickCount64();
    const DWORD graceAge=graceUntil?(DWORD)(nowTick>graceUntil?nowTick-graceUntil:0):0;
    const DWORD wait=ScheduleWaitMs(idle,idleMs,graceUntil!=0,graceAge,dimmed);
    SetCoalescableTimer(window,kCheckTimer,wait,nullptr,1000);
}
static void Check() {
    ++checks;
    // Panel statistics ride the existing scheduler tick; they never add a timer of their own.
    // The app's whole reason to exist is near-zero idle CPU, so nothing here may run when the
    // panel is closed and the screen is at full brightness beyond one add.
    sessionstats::NoteWake();
    {
        const ULONGLONG now=GetTickCount64();
        // Guarded on both terms: before the first dim there is no interval to measure, and while
        // restored the screen is not saving anything.
        if(dimmed&&lastStatsTick)sessionstats::AccumulateDimmed(now-lastStatsTick);
        lastStatsTick=now;
        if(dimmed)SaveStats();
    }
    // An explicit off session is settled: media/idle checks must not restore or re-off it.
    if(displayOff.Active()){ScheduleNext();return;}
    if(saved.count&&!dimmed){Restore();ScheduleNext();return;}
    if(!enabled||locked)return;
    // Cheap path. Not dimmed AND the threshold still in the future means no dim can be due, so
    // neither expensive probe can change any decision. This covers the entire countdown and is
    // the common case - one GetLastInputInfo and a reschedule, no WinRT, no window enumeration.
    if(!dimmed){
        LASTINPUTINFO probe={sizeof(probe),0};
        if(GetLastInputInfo(&probe)){
            const DWORD idle=GetTickCount()-probe.dwTime;
            if(idle<idleMs&&GetTickCount64()<graceUntil){ScheduleNext();return;}
        }
    }
    int playing=(protectMedia||protectSound)?MediaInUse():0;
    bool full=protectFullscreen&&Fullscreen();
    if(!manualDim&&(playing!=0||full)){
        Restore();graceUntil=GetTickCount64()+idleMs;
        // 2 is the audio fallback, kept distinct in the status so the reason is visible without a
        // log dive: a browser with media keys off publishes no SMTC session at all.
        wcscpy(status,playing==1?L"Paused for media playback":playing==2?L"Paused for audio playback":playing<0?L"Media detection unavailable; dimming paused":L"Paused for full-screen app");UpdateTray();ScheduleNext();return;
    }
    LASTINPUTINFO input={sizeof(input),0};if(!GetLastInputInfo(&input)){ScheduleNext();return;}
    DWORD idle=GetTickCount()-input.dwTime;
    if(dimmed&&!manualDim&&idle<idleMs){Restore();graceUntil=GetTickCount64()+idleMs;}
    else if(!dimmed&&ShouldDim(enabled,locked,idle,GetTickCount64(),graceUntil,idleMs,false,false)){
        // The threshold is reached. Which stage runs first is the user's choice: dim (default), or
        // go straight to standby with the brightness untouched.
        if(offInsteadOfDim)StartOffInsteadOfDim();
        else Dim();
    }
    else if(!dimmed){wcscpy(status,L"Enabled; waiting for idle");UpdateTray();}
    ScheduleNext();
}
static bool Autostart() {
    wchar_t value[MAX_PATH+32]={};DWORD bytes=sizeof(value);
    if(RegGetValueW(HKEY_CURRENT_USER,kRunKey,kRunName,RRF_RT_REG_SZ,nullptr,value,&bytes)!=ERROR_SUCCESS)return false;
    wchar_t expected[MAX_PATH+32];swprintf(expected,MAX_PATH+32,L"\"%ls\" --autostart",exe);return wcscmp(value,expected)==0;
}
static void ToggleAutostart() {
    HKEY key;LONG error=RegCreateKeyExW(HKEY_CURRENT_USER,kRunKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(error==ERROR_SUCCESS){
        if(Autostart())error=RegDeleteValueW(key,kRunName);
        else {wchar_t value[MAX_PATH+32];swprintf(value,MAX_PATH+32,L"\"%ls\" --autostart",exe);error=RegSetValueExW(key,kRunName,0,REG_SZ,(BYTE*)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t)));}
        RegCloseKey(key);
    }
    if(error!=ERROR_SUCCESS)MessageBoxW(window,L"Windows could not change the autostart setting.",L"Idle Dimmer",MB_ICONERROR);
}
// ---------------------------------------------------------------------------
// settings.ini: one parser, one writer, no Windows profile API
// ---------------------------------------------------------------------------
// Why the profile API was abandoned: GetPrivateProfileIntW/WritePrivateProfileStringW cannot
// parse a section header behind a UTF-8 BOM. `native\settings.ini` was last written by a
// PowerShell one-liner, which emits a UTF-8 BOM by default; the API skipped the real
// (BOM-prefixed) [Settings] header and appended a second, clean one beside it, so the file
// carried two sections with different values and the effective settings depended on which
// API you asked. The reader below tolerates exactly that file; the writer cannot create it.
//
// The pure half (ParseSettings / FormatSettings) is extracted so --self-test can pin it
// (AGENTS.md section 5). Both are free functions of their input: no globals, no file I/O.
struct Settings {
    bool enabled;
    bool protectMedia;
    bool protectSound;
    bool protectFullscreen;
    DWORD idleMinutes;
    int iconDesign;
    // Minutes to keep dimming before the displays are switched off (0 = never). Separate from
    // idleMinutes because they answer different questions: when to dim, and when to stop
    // emitting light at all. 0 keeps the old behaviour byte-for-byte, so an untouched install
    // never sleeps a monitor it did not sleep before.
    DWORD offMinutes;
    // Switch the displays off AT the idle threshold instead of dimming first. Mutually exclusive
    // with offMinutes (no dim happens, so there is no post-dim delay to measure): when set, the
    // threshold check goes straight to standby and brightness is never touched.
    bool offInsteadOfDim;
    // Live acrylic: capture what is behind the panel, blur and tint it, paint it as the base.
    // Off = the flat plate, and nothing is captured or threaded at all.
    bool acrylic;
    // Live refresh of the material while the panel is open. Off = one snapshot taken before the
    // panel appears, which needs no capture exclusion and keeps screenshots working.
    bool acrylicLive;
};
// The defaults the five startup reads used to hand to the profile API.
static Settings DefaultSettings() {
    Settings s;
    s.enabled=true;s.protectMedia=true;s.protectSound=true;s.protectFullscreen=true;s.idleMinutes=2;s.iconDesign=4;s.offMinutes=0;s.offInsteadOfDim=false;s.acrylic=true;s.acrylicLive=false;
    return s;
}
// ASCII case-insensitive compare of len characters against a NUL-terminated name. Hand-rolled
// because the profile API's section/key matching is case-insensitive and _strnicmp is not
// declared by every MinGW configuration this build is compiled with.
static bool NameEq(const char* text,int len,const char* name) {
    int i=0;
    for(;i<len&&name[i];i++){
        char a=text[i],b=name[i];
        if(a>='A'&&a<='Z')a=(char)(a-'A'+'a');
        if(b>='A'&&b<='Z')b=(char)(b-'A'+'a');
        if(a!=b)return false;
    }
    return i==len&&name[i]==0;
}
// Plain unsigned decimal only. A blank value, "yes", "3x" or a negative sign is unparseable
// and the caller keeps its default: a typo must not silently turn dimming off.
static bool ParseDecimal(const char* text,int len,long& out) {
    int i=0;while(i<len&&(text[i]==' '||text[i]=='\t'))i++;
    int end=len;while(end>i&&(text[end-1]==' '||text[end-1]=='\t'))end--;
    if(i>=end)return false;
    long value=0;
    for(;i<end;i++){
        if(text[i]<'0'||text[i]>'9')return false;
        if(value>100000000L)return false;   // no settings value is near this; guards overflow
        value=value*10+(text[i]-'0');
    }
    out=value;return true;
}
// Pure: raw file bytes in, settings out.
//   - a leading UTF-8 BOM (EF BB BF) is skipped, so the first section is visible at all;
//   - only keys inside a [Settings] section are read;
//   - when the file carries several [Settings] sections the LAST one wins, which is the
//     section the profile API actually read - so no value the user chose is reinterpreted;
//   - a missing or unparseable key keeps its default;
//   - the numeric keys are clamped exactly as the startup reads always clamped them, and the
//     booleans keep the !=0 truthiness the profile API produced.
static Settings ParseSettings(const char* text) {
    Settings out=DefaultSettings();
    if(!text)return out;
    const char* p=text;
    if((unsigned char)p[0]==0xEF&&(unsigned char)p[1]==0xBB&&(unsigned char)p[2]==0xBF)p+=3;
    bool inSettings=false;
    while(*p){
        const char* start=p;
        while(*p&&*p!='\n'&&*p!='\r')p++;
        int len=(int)(p-start);
        if(*p=='\r'&&p[1]=='\n')p+=2;else if(*p)p++;   // CRLF, LF or a lone CR all end a line
        int first=0;while(first<len&&(start[first]==' '||start[first]=='\t'))first++;
        int last=len;while(last>first&&(start[last-1]==' '||start[last-1]=='\t'))last--;
        if(last-first>=2&&start[first]=='['&&start[last-1]==']'){
            // A fresh [Settings] header restarts from the defaults, so the last section wins
            // whole - including what it omits, which is what "missing key returns the default"
            // has to mean for the section that is in effect.
            if(NameEq(start+first+1,(last-first)-2,"Settings")){out=DefaultSettings();inSettings=true;}
            else inSettings=false;
            continue;
        }
        if(!inSettings)continue;
        int equals=-1;
        for(int i=first;i<last;i++)if(start[i]=='='){equals=i;break;}
        if(equals<0)continue;
        int keyEnd=equals;while(keyEnd>first&&(start[keyEnd-1]==' '||start[keyEnd-1]=='\t'))keyEnd--;
        int valueFirst=equals+1;while(valueFirst<last&&(start[valueFirst]==' '||start[valueFirst]=='\t'))valueFirst++;
        int keyLen=keyEnd-first,valueLen=last-valueFirst;
        long value=0;
        if(NameEq(start+first,keyLen,"Enabled")){if(ParseDecimal(start+valueFirst,valueLen,value))out.enabled=value!=0;}
        else if(NameEq(start+first,keyLen,"ProtectMedia")){if(ParseDecimal(start+valueFirst,valueLen,value))out.protectMedia=value!=0;}
        else if(NameEq(start+first,keyLen,"ProtectSound")){if(ParseDecimal(start+valueFirst,valueLen,value))out.protectSound=value!=0;}
        else if(NameEq(start+first,keyLen,"ProtectFullscreen")){if(ParseDecimal(start+valueFirst,valueLen,value))out.protectFullscreen=value!=0;}
        else if(NameEq(start+first,keyLen,"IdleMinutes")){if(ParseDecimal(start+valueFirst,valueLen,value))out.idleMinutes=ClampIdleMinutes(value);}
        else if(NameEq(start+first,keyLen,"Icon")){if(ParseDecimal(start+valueFirst,valueLen,value))out.iconDesign=ClampIconDesign(value);}
        else if(NameEq(start+first,keyLen,"OffMinutes")){if(ParseDecimal(start+valueFirst,valueLen,value))out.offMinutes=ClampOffMinutes(value);}
        else if(NameEq(start+first,keyLen,"OffInsteadOfDim")){if(ParseDecimal(start+valueFirst,valueLen,value))out.offInsteadOfDim=value!=0;}
        else if(NameEq(start+first,keyLen,"Acrylic")){if(ParseDecimal(start+valueFirst,valueLen,value))out.acrylic=value!=0;}
        else if(NameEq(start+first,keyLen,"AcrylicLive")){if(ParseDecimal(start+valueFirst,valueLen,value))out.acrylicLive=value!=0;}
    }
    return out;
}
// The only two ways bytes reach the canonical text, so there is one implementation of
// "canonical" and the formatter cannot emit a BOM, a second section, an LF or another order.
static int AppendText(char* out,int cap,int n,const char* text) {
    if(n<0)return -1;
    for(;*text;text++){if(n>=cap-1)return -1;out[n++]=*text;}
    return n;
}
static int AppendDecimal(char* out,int cap,int n,long value) {
    if(n<0)return -1;
    char digits[16];int k=0;
    if(value<0)value=0;
    if(!value)digits[k++]='0';
    while(value){digits[k++]=(char)('0'+value%10);value/=10;}
    while(k){if(n>=cap-1)return -1;out[n++]=digits[--k];}
    return n;
}
// Pure: settings out, canonical file text in. Exactly one [Settings] section, no BOM, CRLF
// line endings, keys in a fixed order. Returns the byte count written (excluding the NUL), or
// 0 when the buffer is too small.
static int FormatSettings(const Settings& st,char* out,int cap) {
    static const char* keys[10]={"Enabled","ProtectMedia","ProtectSound","ProtectFullscreen","IdleMinutes","Icon","OffMinutes","OffInsteadOfDim","Acrylic","AcrylicLive"};
    const long values[10]={(long)(st.enabled?1:0),(long)(st.protectMedia?1:0),(long)(st.protectSound?1:0),(long)(st.protectFullscreen?1:0),(long)st.idleMinutes,(long)st.iconDesign,(long)st.offMinutes,(long)(st.offInsteadOfDim?1:0),(long)(st.acrylic?1:0),(long)(st.acrylicLive?1:0)};
    if(!out||cap<2)return 0;
    int n=AppendText(out,cap,0,"[Settings]\r\n");
    for(int i=0;i<10;i++){
        n=AppendText(out,cap,n,keys[i]);
        if(n>=0)n=AppendText(out,cap,n,"=");
        if(n>=0)n=AppendDecimal(out,cap,n,values[i]);
        if(n>=0)n=AppendText(out,cap,n,"\r\n");
    }
    if(n<0)return 0;
    out[n]=0;
    return n;
}
// Canonical means byte-for-byte equal to what FormatSettings writes for the values the file
// itself parses to. One comparison therefore covers a BOM, a duplicated [Settings], LF
// endings, reordered keys and stray whitespace.
static bool SettingsTextIsCanonical(const char* text) {
    if(!text)return false;
    char canonical[256];
    int n=FormatSettings(ParseSettings(text),canonical,sizeof(canonical));
    if(n<=0)return false;
    int len=0;while(text[len])len++;
    if(len!=n)return false;
    for(int i=0;i<n;i++)if(text[i]!=canonical[i])return false;
    return true;
}
// Raw bytes, deliberately unparsed: the BOM must reach SettingsTextIsCanonical intact,
// because that is how a non-canonical file is recognised and healed.
static bool ReadSettingsText(char* out,int cap) {
    if(!out||cap<2)return false;
    out[0]=0;
    HANDLE f=CreateFileW(ini,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD size=GetFileSize(f,nullptr),read=0;
    bool ok=size!=INVALID_FILE_SIZE&&size<(DWORD)(cap-1);
    if(ok)ok=ReadFile(f,out,size,&read,nullptr)!=FALSE&&read==size;
    CloseHandle(f);
    if(!ok){out[0]=0;return false;}
    out[read]=0;
    return true;
}
// The whole file, always. Written as raw bytes to a sibling temp file and renamed over the
// target, so a crash or a full disk mid-write leaves the previous settings.ini intact instead
// of truncated. CREATE_ALWAYS writes no BOM - a CRT text-mode open is what would add one.
static bool WriteSettingsFile(const Settings& st) {
    char text[256];
    int n=FormatSettings(st,text,sizeof(text));
    if(n<=0)return false;
    wchar_t temp[MAX_PATH];
    swprintf(temp,MAX_PATH,L"%ls.tmp",ini);
    HANDLE f=CreateFileW(temp,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD wrote=0;
    bool ok=WriteFile(f,text,(DWORD)n,&wrote,nullptr)!=FALSE&&wrote==(DWORD)n;
    if(ok)ok=FlushFileBuffers(f)!=FALSE;
    CloseHandle(f);
    if(ok)ok=MoveFileExW(temp,ini,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok)DeleteFileW(temp);
    return ok;
}
// The in-memory state, clamped on the way out exactly as the read path clamps on the way in,
// so no caller can write a file the startup read would interpret differently.
static Settings CurrentSettings() {
    Settings st;
    st.enabled=enabled;st.protectMedia=protectMedia;st.protectSound=protectSound;st.protectFullscreen=protectFullscreen;
    st.idleMinutes=ClampIdleMinutes((long)(idleMs/60000));
    st.iconDesign=ClampIconDesign(iconDesign);
    // Every field must be assigned: an unset member here is an indeterminate stack value, and the
    // startup heal wrote one of 411 into settings.ini before this line existed. Any new key added
    // to Settings has to be added here too.
    st.offMinutes=ClampOffMinutes((long)offMinutes);
    st.offInsteadOfDim=offInsteadOfDim;
    st.acrylic=acrylicEnabled;
    st.acrylicLive=acrylicLiveEnabled;
    return st;
}
// Same name and same callers as before: every toggle, slider commit and icon pick lands here.
static void SaveSettings() { WriteSettingsFile(CurrentSettings()); }
static void ApplyEnabled() {
    if(enabled){
        if(!SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED)){
            enabled=false;ErrorStatus(L"Keep-awake request failed",HRESULT_FROM_WIN32(GetLastError()));
        }else{
            // Grace must be armed BEFORE the first schedule: ScheduleNext() reads graceUntil,
            // and with graceUntil==0 the cheap-path guard `now < graceUntil` fails and the app
            // would dim on its very first wake instead of waiting out a full idle period.
            graceUntil=GetTickCount64()+idleMs;ScheduleNext();
            wcscpy(status,L"Enabled; waiting for idle");
        }
    }
    if(!enabled){Restore();SetThreadExecutionState(ES_CONTINUOUS);if(!saved.count)KillTimer(window,kCheckTimer);wcscpy(status,L"Disabled; normal Windows power settings");}
    SaveSettings();UpdateTray();
}
static void Balloon(const wchar_t* title,const wchar_t* text) {
    tray.uFlags=NIF_INFO;wcsncpy(tray.szInfoTitle,title,63);wcsncpy(tray.szInfo,text,255);tray.dwInfoFlags=NIIF_INFO;Shell_NotifyIconW(NIM_MODIFY,&tray);
}
// IconPreview stays because the panel's icon picker is still a native popup. Nothing else
// in the app builds a menu now; this forward declaration is the last of the old menu's
// reach and it is deliberate - see ActivatePanelTarget().
static HBITMAP IconPreview(int design,bool picked);
static HICON MakeIcon(COLORREF color,bool crescent) {
    BITMAPV5HEADER header={};header.bV5Size=sizeof(header);header.bV5Width=32;header.bV5Height=-32;header.bV5Planes=1;header.bV5BitCount=32;header.bV5Compression=BI_BITFIELDS;
    header.bV5RedMask=0x00ff0000;header.bV5GreenMask=0x0000ff00;header.bV5BlueMask=0x000000ff;header.bV5AlphaMask=0xff000000;
    void* pixels=nullptr;HDC dc=GetDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,(BITMAPINFO*)&header,DIB_RGB_COLORS,&pixels,nullptr,0);ReleaseDC(nullptr,dc);
    if(!bitmap)return CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));
    DWORD* p=(DWORD*)pixels;DWORD rgb=(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
    for(int y=0;y<32;y++)for(int x=0;x<32;x++){
        int dx=x-15,dy=y-15;bool draw=dx*dx+dy*dy<=81;
        if(crescent){int cx=x-20,cy=y-11;if(cx*cx+cy*cy<64)draw=false;}
        else if((x>=14&&x<=16&&(y<4||y>26))||(y>=14&&y<=16&&(x<4||x>26)))draw=true;
        p[y*32+x]=draw?(0xff000000|rgb):0;
    }
    HBITMAP mask=CreateBitmap(32,32,1,1,nullptr);ICONINFO info={TRUE,0,0,mask,bitmap};HICON icon=CreateIconIndirect(&info);DeleteObject(mask);DeleteObject(bitmap);return icon;
}
// Theme state follows the documented SystemUsesLightTheme value: 1 = light Windows (taskbar) colors.
static bool TaskbarIsLight() {
    DWORD value=0,size=sizeof(value);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)!=ERROR_SUCCESS)return false;
    return value!=0;
}
// Every DWM entry point is resolved by name from a dynamically loaded dwmapi.dll. The
// project forbids new runtime dependencies, and attributes 33/38 do not exist before
// Windows 11, so a hard link against dwmapi would be both a new dependency and a launch
// failure on older builds. Missing attributes degrade to the tier-3 opaque background
// PaintPanel already paints first. Verified on this machine by tools\check-dwm.cpp:
// acrylic and rounded corners are available.
//
// DwmGetColorizationColor is declared here, above SystemAccent(), because the accent reads it. In a
// single translation unit declaration order is not a style preference - it is the compile order - so
// this block sits next to its first caller rather than with the panel work further down.
typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND,DWORD,LPCVOID,DWORD);
typedef HRESULT(WINAPI* DwmGetColorizationColorFn)(DWORD*,DWORD*);
static DwmSetWindowAttributeFn DwmSetAttr=nullptr;
static DwmGetColorizationColorFn DwmGetColor=nullptr;
static bool DwmResolved=false;
static void ResolveDwm() {
    if(DwmResolved)return;
    DwmResolved=true;
    HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
    if(dwm){
        DwmSetAttr=(DwmSetWindowAttributeFn)GetProcAddress(dwm,"DwmSetWindowAttribute");
        DwmGetColor=(DwmGetColorizationColorFn)GetProcAddress(dwm,"DwmGetColorizationColor");
    }
}
static bool SetDwmAttr(HWND h,DWORD attr,LPCVOID value,DWORD size) {
    if(!DwmSetAttr)return false;
    return SUCCEEDED(DwmSetAttr(h,attr,value,size));
}
// The system accent colour, as 0x00RRGGBB.
//
// Source: HKCU\...\Explorer\Accent\AccentPalette, entry 1. MEASURED 2026-10-24 on this machine
// (dark theme), the eight entries are, light to dark:
//
//   [0] #ECDCAA   [1] #D4BB8B   [2] #B28249   [3] #986B3E
//   [4] #7F5330   [5] #60361F   [6] #3C130B   [7] #881798
//
// Entry 1 is the resting fill of an ON Quick Settings tile: the user measured #D4BB8B off
// Windows' own panel. Entry 0 is the lightest shade in the ramp - what a tile shows while the
// pointer is over it - and is NOT the tile fill.
//
// The previous version of this function read entry 0 and asserted in this very comment that entry
// 0 was "the accent" and "matches Windows' own tiles exactly" (#EADFCD). That was false, and it is
// the fifth wrong accent in this file. The first four were wrong sources or wrong channel order;
// this one was the right key read at the wrong index, which is harder to notice because a
// neighbouring swatch is always a plausible colour. The value looked authoritative because the
// measurement that produced it was taken with a pointing tool resting on a hovered tile, so it
// read back a highlight. Entries 0 and 1 differ by (+24,+33,+31) per channel - a shade, not a byte
// swap - so no channel test could catch it. Only comparing against the real panel did.
//
// Precedence. One real source and one fallback - which is a reduction, not an expansion.
//
// Three sources were tried before the palette, and each looked plausible:
//
//   1. Themes\Personalize\AccentColor. MEASURED 2026-10-19, this machine: the key does not exist.
//      Windows 11 never writes it, so this rung could never fire.
//   2. GetSysColor(COLOR_HIGHLIGHT). A Win16-era metric Windows 11 no longer maintains. MEASURED
//      2026-10-19, this machine: 0x000000 on one call, a plausible ORANGE on another in the same
//      session. Because the orange is non-zero it also *suppressed* the safe fallback below, which
//      is how the panel painted orange tiles while Windows showed the real accent.
//   3. DwmGetColorizationColor. A real, documented DWM API, and still the wrong answer: it is the
//      Aero colourisation tint used for window BORDERS, not the tile accent. MEASURED 2026-10-19:
//      0x2B271E1A, nothing like the palette. It was chosen because "it is the API Windows calls",
//      which is reasoning from familiarity, not from measurement.
//
// The common failure is worth naming: an unreliable value is worse than no value, because every
// guard here keys on "non-zero". Each of those passed that check. Only comparing the rendered
// pixel against Windows' own Quick Settings catches it, which is what the user did.
//
// Each entry is R,G,B,A in memory order. The palette is deliberately NOT read through GetRValue:
// COLORREF packs red in the LOW byte, so the same bytes reinterpreted as a COLORREF give #EA0000 -
// a red with no green or blue. The palette is a packed RGB triple, and the COLORREF helpers would
// silently drop two channels. AccentChecks() pins both readings so the distinction cannot be
// reintroduced by accident.
static unsigned int AccentFromPalette(const BYTE* palette,int count,int index) {
    // One entry is four bytes: R, G, B, A. The index is explicit so entry 0 can never be
    // reached by accident through a call that meant "the tile fill".
    if(!palette||index<0)return 0;
    const int offset=index*4;
    if(count<offset+3)return 0;
    return ((unsigned int)palette[offset]<<16)|((unsigned int)palette[offset+1]<<8)|palette[offset+2];
}
static unsigned int AccentFromColorRef(DWORD colorRef) {
    return (unsigned int)(((GetRValue(colorRef)<<16))|((GetGValue(colorRef)<<8))|GetBValue(colorRef));
}
// One entry of the AccentPalette, or 0 when the value is absent or too short for that index.
// The palette is a fixed 32 bytes, so a stack buffer avoids a heap round-trip on a path that runs
// on every paint. A value larger than the buffer fails the read and falls back, which is the same
// outcome as an absent key - and cannot happen with the shape Windows writes.
static unsigned int AccentEntry(int index) {
    const wchar_t* key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";
    BYTE palette[64];
    DWORD size=sizeof(palette);
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"AccentPalette",RRF_RT_REG_BINARY,nullptr,palette,&size)!=ERROR_SUCCESS)
        return 0;
    return AccentFromPalette(palette,(int)size,index);
}
// Which rung produced the accent, for --diagnose. Reporting the source alongside the value is
// what makes a colour mismatch diagnosable instead of arguable.
static const wchar_t* g_accentSource=L"fallback";
static unsigned int SystemAccent() {
    // Entry 1 is the resting tile fill. Entry 0 is the lightest shade - the highlight - and is
    // only a fallback for a palette too short to hold a second entry; --diagnose names whichever
    // answered, so a short-palette machine cannot silently paint the highlight as the fill.
    unsigned int rgb=AccentEntry(1);
    if(rgb){g_accentSource=L"AccentPalette[1]";return rgb;}
    rgb=AccentEntry(0);
    if(rgb){g_accentSource=L"AccentPalette[0] (short palette)";return rgb;}
    // The design document's own documented fallbacks, which until this was fixed were compiled and
    // referenced nowhere. A panel with no accent is worse than one with the wrong accent: the first
    // renders black-on-black and is unreadable.
    const Color fallback=TaskbarIsLight()?theme::accentLight():theme::accentDark();
    g_accentSource=L"theme fallback";
    return ((unsigned int)fallback.r<<16)|((unsigned int)fallback.g<<8)|fallback.b;
}
// The highlight swatch: entry 0, the colour Windows paints on a hovered tile. MEASURED 2026-10-24
// on this machine: an ON tile rests at #D4BB8B (entry 1) and highlights to #ECDCAA (entry 0). 0
// means unavailable, and PaintPanel then derives a per-channel lift so a hovered tile still
// responds rather than looking dead.
static unsigned int SystemAccentHover() {
    return AccentEntry(0);
}
// The notification area draws each glyph into a 16 logical px box, which is SM_CXSMICON - the
// metric Microsoft documents for small icons and the one LIM_SMALL maps to. Supplying a larger
// HICON is not ignored, just resampled: handing the shell 48 px at 200% scaling produced a glyph
// identical to the 32 px frame scaled by 32/48, so the extra pixels only cost sharpness.
// Ask for exactly SM_CXSMICON, from the monitor that actually hosts the taskbar so a second
// display at a different scale still resolves correctly.
typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
typedef int(WINAPI* GetSystemMetricsForDpiFn)(int,UINT);
static int TrayIconSize() {
    static GetDpiForWindowFn dpiForWindow=nullptr;
    static GetSystemMetricsForDpiFn metricsForDpi=nullptr;
    static bool resolved=false;
    if(!resolved){
        HMODULE user=GetModuleHandleW(L"user32.dll");
        if(user){
            dpiForWindow=(GetDpiForWindowFn)GetProcAddress(user,"GetDpiForWindow");
            metricsForDpi=(GetSystemMetricsForDpiFn)GetProcAddress(user,"GetSystemMetricsForDpi");
        }
        resolved=true;
    }
    int dpi=0;
    HWND shell=GetShellWindow(); // The taskbar itself; may live on a secondary monitor.
    if(shell&&dpiForWindow)dpi=(int)dpiForWindow(shell);
    if(dpi<=0){HDC dc=GetDC(nullptr);if(dc){dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);}}
    if(dpi<=0)dpi=96;
    int px=metricsForDpi?metricsForDpi(SM_CXSMICON,(UINT)dpi):MulDiv(SM_CXSMICON,dpi,96);
    return px<16?16:px;
}
static HICON LoadStateIcon(int design,bool light,int px,const wchar_t* state) {
    wchar_t path[MAX_PATH+64];
    swprintf(path,MAX_PATH+64,L"%ls\\icons\\tray%d-%ls-%ls.ico",folder,design,light?L"onlight":L"ondark",state);
    return (HICON)LoadImageW(nullptr,path,IMAGE_ICON,px,px,LR_LOADFROMFILE);
}
// Applies the Fluent look. Each attribute is independent: a build that lacks one keeps the
// others, and anything missing is already covered by the background PaintPanel draws.
static void StylePanel(HWND h) {
    ResolveDwm();
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 1809)
    const BOOL dark=!taskbarLight;
    SetDwmAttr(h,20,&dark,sizeof(dark));
    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE; 2 = DWMWCP_ROUND (Win11 21H2)
    const int round=2;
    PanelHasCorners=SetDwmAttr(h,33,&round,sizeof(round));
    // The native session enables the host backdrop. Do not request a second DWM material plate.
    // Extend the frame so the rounded corners and shadow are composited by DWM.
    struct { long cxLeftWidth, cxRightWidth, cyTopHeight, cyBottomHeight; } m = {0,0,0,0};
    HMODULE user=GetModuleHandleW(L"user32.dll");
    typedef HRESULT(WINAPI* ExtendFn)(HWND,void*);
    ExtendFn extend=user?(ExtendFn)GetProcAddress(user,"DwmExtendFrameIntoClientArea"):nullptr;
    if(extend)extend(h,&m);
}

// Panel interaction state. Module-scope rather than window-property because the panel has
// exactly one instance and these are per-gesture, not persisted.
//
// Declared ABOVE CurrentPanelState because that function copies them into the state handed to the
// painter. They used to be declared further down, after it, and were therefore never copied at
// all: the panel tracked the hovered control faithfully and then painted every frame with
// hover=hitNone, so no tile could highlight. Declaration order is compile order in a single
// translation unit, and this omission is what that costs.
static int panelHover=hitNone, panelPressed=hitNone;

// The panel's current view of the app, rebuilt from live state on every paint so it can
// never show something stale.
static void CurrentPanelState(PanelState& s) {
    s.dark=!taskbarLight;   // taskbarLight==true means a light taskbar, so invert
    s.enabled=enabled;
    s.dimmed=dimmed;
    s.mediaGuard=protectMedia;
    s.soundGuard=protectSound;
    s.fullscreenGuard=protectFullscreen;
    s.offMode=offInsteadOfDim;
    s.autostart=Autostart();
    s.minutes=(int)(idleMs/60000);
    LASTINPUTINFO input={sizeof(input),0};
    s.idleSec=GetLastInputInfo(&input)?(int)((GetTickCount()-input.dwTime)/1000):0;
    s.design=iconDesign;
    s.accent=SystemAccent();
    s.accentHover=SystemAccentHover();
    // Interaction state. Without these two lines the painter sees hitNone on every frame, so the
    // hover highlight and the press step are computed and never drawn.
    s.hover=panelHover;
    s.pressed=panelPressed;
    // Session counters for the power dashboard card. Read at paint time from a module that never
    // touches the clock itself, so this is a struct copy on a path that already repaints.
    {
        const sessionstats::Counters k=sessionstats::Get();
        // "Wake count" means wakes from the dimmed state, not scheduler wakeups. restoreCount is
        // exactly that: Restore() only increments after a dimmed screen was actually returned to
        // full brightness, so each recorded dim pairs with a wake. The scheduler's own wake
        // counter is still recorded in stats.bin for diagnostics, but the card does not show it.
        s.wakesFromDim=k.restoreCount;
        s.dimmedMs=k.dimmedMs;
        // Session-scoped numerator for the card's progress bar. dimmedMs is seeded from disk,
        // so it is a lifetime total; subtracting the startup base leaves only time this
        // process spent dimmed, which is what the per-process uptime denominator describes.
        s.sessionDimmedMs=k.dimmedMs>sessionDimmedBaseMs?k.dimmedMs-sessionDimmedBaseMs:0ULL;
        s.hasDimHistory=sessionstats::HasDimHistory(k)||(persistedLastDimWallMs!=0ULL);
        // "Last dim" is described by ONE clock: the wall-clock stamp written by Dim(). An earlier
        // version used GetTickCount64() for a dim in this session and the wall clock otherwise,
        // so the same event was described by two different clocks depending on when the panel was
        // opened. A tick resets at boot; the wall stamp is the only value that survives one. The
        // row shows a plain minute count, so no timezone conversion is involved.
        s.msSinceDim=persistedLastDimWallMs?WallDeltaMs(WallNowMs(),persistedLastDimWallMs):0ULL;
        s.lastDimKnown=(persistedLastDimWallMs!=0ULL);
    }
    s.sessionMs=GetTickCount64()-processStartTick;
    // Six tiles: the four protections, the displays-off policy and the login toggle. The layout
    // already reflows to two rows from five buttons upward, so this is the 3+3 arrangement the
    // geometry tests have always covered.
    s.buttonCount=6;
}
static int panelDpi(HWND h) {
    // Derived from the CLIENT WIDTH first, because the window is sized from the same relationship
    // (AnchorPanel uses this dpi to compute r.w = MulDiv(kPanelWidth, dpi, 96)). Asking the OS
    // instead made the painter lay out at a lower DPI than the window it had been given - the
    // content occupied the top two thirds of the panel with dead space below, which only became
    // visible once ordinary screenshots of the panel were possible again.
    if(h){
        RECT rc;
        if(GetClientRect(h,&rc)&&rc.right>0){
            const int derived=MulDiv(rc.right,96,kPanelWidth);
            if(derived>=96)return derived;
        }
    }
    typedef UINT(WINAPI* DpiFn)(HWND);
    static DpiFn fn=nullptr;
    static bool resolved=false;
    if(!resolved){
        HMODULE user=GetModuleHandleW(L"user32.dll");
        if(user)fn=(DpiFn)GetProcAddress(user,"GetDpiForWindow");
        resolved=true;
    }
    if(fn&&h){UINT d=fn(h);if(d)return (int)d;}
    HDC dc=GetDC(nullptr);
    int d=dc?GetDeviceCaps(dc,LOGPIXELSY):96;
    if(dc)ReleaseDC(nullptr,dc);
    return d>0?d:96;
}

// Applies a threshold from the slider. Guarded so a drag can never produce an illegal value:
// the clamp here is the only thing standing between a stray coordinate and a zero threshold.
static void ApplyThreshold(int minutes) {
    DWORD m=ClampIdleMinutes(minutes);
    if(m*60000==idleMs)return;
    idleMs=m*60000;
    wcscpy(status,L"Enabled; waiting for idle");
    UpdateTray();
}

static void Shutdown();
static void LoadIcons();
static void HidePanel(bool clearTrayPair=true);
static void DismissPanel();
// Drop the half-click pairing state (see ResolveTrayGesture). The pairing window must only
// survive between the two halves of ONE click. If the panel was closed by a click away, the
// user's next click on the icon must toggle even if it lands inside that window - otherwise a
// quick click-away-then-click is swallowed and the panel does nothing, which is the same class
// of bug as the flicker this replaced.
static void ForgetLastToggle();

// The 3-second hardware dim/restore test. One implementation for both entry points: the
// panel's "Dim now" button and --test-brightness (which posts kDimTest). The panel used to
// inline a near-copy that omitted the testExit cleanup branch, so an interactive test and a
// scripted one verified different code - exactly the drift the single-UI change is meant to
// end.
static void StartDimTest() {
    if(!Restore())return;
    testing=true;
    if(Dim()){testBefore=saved;testBeforeDdc=ddcSaved;SetTimer(window,kTestTimer,3000,nullptr);return;}
    testing=false;
    if(testExit){WriteRuntimeReport();Shutdown();}
}

// Maps a panel target onto the app's existing behaviour, so a toggle from the panel runs the
// same persistence and side effects it always did - same SaveSettings, same Check, same
// autostart registry write, same brightness-restore paths.
static void DimManual() {
    HidePanel();
    if(dimmed)return;
    manualDim=true;
    manualDimIgnoreUntil=GetTickCount64()+2000ULL;
    if(Dim()){ScheduleNext();return;}
    manualDim=false;
    manualDimIgnoreUntil=0ULL;
}
static bool panelDragging=false;
static SliderMagnet panelSliderMagnet={};
static void ActivatePanelTarget(int hit) {
    switch(hit){
    case hitTileDimming:  enabled=!enabled;ApplyEnabled();break;
    case hitTileMedia:    protectMedia=!protectMedia;SaveSettings();Check();break;
    case hitTileSound:    protectSound=!protectSound;SaveSettings();Check();break;
    case hitTileOffMode:  offInsteadOfDim=!offInsteadOfDim;SaveSettings();Check();break;
    case hitTileFullscreen: protectFullscreen=!protectFullscreen;SaveSettings();Check();break;
    case hitTileAutostart: ToggleAutostart();break;
    case hitTileFolder:   ShellExecuteW(nullptr,L"open",folder,nullptr,nullptr,SW_SHOWNORMAL);break;
    case hitTileIcon:     {
        // The icon picker is the one place a native menu survives, and it is kept on purpose.
        // It is a rare, one-shot choice; IconPreview() already renders the per-design
        // bitmaps correctly against the system menu colour, and a panel-native picker would
        // be a second preview implementation free to disagree with the first. It reports its
        // choice straight from TPM_RETURNCMD, so it needs no command routing.
        POINT p;GetCursorPos(&p);
        HMENU menu=CreatePopupMenu();
        HBITMAP previews[kIconDesignCount]={};
        for(int i=0;i<kIconDesignCount;i++){
            previews[i]=IconPreview(i+1,iconDesign==i+1);
            if(previews[i])AppendMenuW(menu,(i?MF_MENUBARBREAK:0)|MF_BITMAP,kIconFirst+i,(LPCWSTR)previews[i]);
            else AppendMenuW(menu,(i?MF_MENUBARBREAK:0)|MF_STRING,kIconFirst+i,kIconNames[i]);
        }
        SetForegroundWindow(window);
        UINT sel=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,p.x,p.y,0,window,nullptr);
        DestroyMenu(menu);
        for(int i=0;i<kIconDesignCount;i++)if(previews[i])DeleteObject(previews[i]);
        PostMessageW(window,WM_NULL,0,0);
        if(sel&&sel>=kIconFirst&&sel<kIconFirst+kIconDesignCount){
            iconDesign=(int)(sel-kIconFirst)+1;
            SaveSettings();LoadIcons();UpdateTray();
            if(panelWindow)InvalidateRect(panelWindow,nullptr,FALSE);
        }
        break;
    }
    case hitDimNow: DimManual();break;
    // Manual off is independent of brightness controls and media protection.
    case hitDisplaysOff: HidePanel();OffDisplays(true);break;
    case hitFooterExit: HidePanel();Shutdown();break;
    }
}
// Click-away dismissal: a WH_MOUSE_LL low-level mouse hook, installed only while the panel is
// open. This is the platform API for "the user clicked somewhere that is not me" - there is no
// message for it, because a click outside the panel is simply delivered to whatever window is
// underneath and this app never sees it.
//
// Two earlier attempts used a 1 s cursor-position poll behind a "must have been hovered once
// first" guard flag; that guard is why clicking straight away never dismissed the panel.
// A third tried WM_ACTIVATE/WA_INACTIVE, which requires the panel to take focus - and it
// cannot: SetForegroundWindow from a tray app loses to the shell, so the panel activated and
// was then immediately deactivated, hiding itself within a second of opening. Measured, not
// assumed: `--menu` opened it and `--status` reported it hidden 2 s later with no input.
//
// The hook must stay fast. Windows removes a low-level hook whose callback exceeds
// LowLevelHooksTimeout, and it does so silently. The callback therefore does no I/O at all:
// it evaluates RectContainsPoint (pinned in --self-test via Flyout.h) and posts
// kPanelDismiss; the actual teardown happens on our own message loop. The click is NOT
// swallowed - it belongs to the window underneath, exactly as it would for any menu.
// The callback is a pure predicate plus at most one PostMessage, and nothing else. It used to
// also format a string and open, append to and close a file on every outside click; that is
// exactly the kind of unbounded latency inside a low-level hook that Windows answers by
// silently removing the hook with no error anywhere, which presents as "click-away quietly
// stopped working". The hook is now the smallest thing that can still do its job.
static LRESULT CALLBACK PanelMouseHook(int code, WPARAM wp, LPARAM lp) {
    if(code==HC_ACTION&&(wp==WM_LBUTTONDOWN||wp==WM_RBUTTONDOWN||wp==WM_MBUTTONDOWN)){
        const MSLLHOOKSTRUCT* hs=reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        POINT pt={hs->pt.x,hs->pt.y};
        // Bail out before anything is queued if the panel is not up. A double-click delivers
        // two WM_LBUTTONDOWNs, so the first dismissal is still in the queue when the second
        // arrives; this is the first of the two guards that keeps that harmless.
        if(panelWindow&&IsWindowVisible(panelWindow)&&!RectContainsPoint(panelBounds,pt)){
            PostMessage(panelWindow,kPanelDismiss,(WPARAM)pt.x,(LPARAM)pt.y);
        }
    }
    return CallNextHookEx(panelHook,code,wp,lp);
}
// Is the foreground window owned by a capture tool? Used to freeze automatically, because while
// acrylic is live the panel is excluded from capture and the tools refuse to start otherwise.
static bool ForegroundIsCaptureTool() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return acrylic::IsCaptureTool(base);
}

static void HidePanel(bool clearTrayPair) {
    if(!panelWindow){panelOpening=false;return;}
    // Live acrylic ends with the panel: the capture thread exits and the exclusion is cleared, so
    // nothing is running (and nothing is hidden from screenshots) while the UI is absent.
    if(clearTrayPair)ForgetLastToggle();
    if(panelHook){UnhookWindowsHookEx(panelHook);panelHook=nullptr;}
    KillTimer(panelWindow,kPanelTimer);
    KillTimer(panelWindow,kPanelMotionTimer);
    KillTimer(panelWindow,kCaptureWatchTimer);
    panelOpening=false;
    panelEntering=false;
    panelExiting=false;
    // Second guard against the double-click's second dismissal: HidePanel is idempotent, so
    // the extra post costs nothing.
    if(IsWindowVisible(panelWindow))ShowWindow(panelWindow,SW_HIDE);
    nativebackdrop::Stop();
    acrylic::Stop();
}
static void DismissPanel() {
    if(!panelWindow||!IsWindowVisible(panelWindow)||panelExiting)return;
    ForgetLastToggle();
    if(panelHook){UnhookWindowsHookEx(panelHook);panelHook=nullptr;}
    KillTimer(panelWindow,kPanelTimer);
    if(GetCapture()==panelWindow)ReleaseCapture();
    BOOL animations=FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
    if(!animations){HidePanel();return;}
    if(nativebackdrop::Running()){
        panelEntering=false;
        panelExiting=true;
        if(!nativebackdrop::AnimateHide())HidePanel(false);
        return;
    }
    // Capture the position already displayed, so closing midway through entry never jumps.
    RECT current=panelBounds;
    GetWindowRect(panelWindow,&current);
    panelExitFromOffset=current.top-panelTarget.y;

    panelEntering=false;
    panelExiting=true;
    panelEntranceStart=GetTickCount64();
    if(!SetTimer(panelWindow,kPanelMotionTimer,16,nullptr))HidePanel();
}
// Only runs during a transition. Wall time controls progress so a delayed frame cannot
// lengthen the animation; no sleep, worker thread, or global timer-resolution change.
static void PanelMotionTick(HWND h) {
    if(!panelEntering&&!panelExiting)return;
    if(!IsWindowVisible(h)){
        KillTimer(h,kPanelMotionTimer);
        panelEntering=false;
        panelExiting=false;
        return;
    }
    const ULONGLONG elapsed=GetTickCount64()-panelEntranceStart;
    const int offset=panelExiting?PanelExitOffset(elapsed,panelExitFromOffset,panelEntranceDpi)
                                 :PanelEntranceOffset(elapsed,panelEntranceDpi);
    const int y=panelTarget.y+offset;
    SetWindowPos(h,nullptr,panelTarget.x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    panelBounds=RECT{panelTarget.x,y,panelTarget.x+panelTarget.w,y+panelTarget.h};
    // The blur follows the window as it slides, so the frost is aligned with what is actually
    // behind it rather than with where it will end up.
    if(acrylic::Running())acrylic::SetCaptureRect(panelBounds);
    if(elapsed>=(panelExiting?kPanelExitMs:kPanelEntranceMs)){
        if(panelExiting){HidePanel(false);return;}
        KillTimer(h,kPanelMotionTimer);
        panelEntering=false;
    }
}
// The 1-second tick exists only while the panel is visible. It drives the live countdown
// and, while open, also advances the tray glyph smoothly - the same 87 us redraw measured
// in tools\bench-icon.cpp. KillTimer on hide is what keeps the closed-panel cost at zero,
// so this is the single line that protects the app's efficiency claim.
// Dismissal when clicking away is handled by the WH_MOUSE_LL hook (PanelMouseHook above),
// not by focus. See the note there for why focus cannot work for a tray flyout.
//
// The 1 s tick below animates the panel. It deliberately holds no dismissal logic; an
// earlier version polled the cursor position here and required the pointer to have been
// "inside once" before closing, which is precisely why clicking straight away never
// dismissed the panel.
static void PanelTick(HWND h) {
    // Animation only. Dismissal lives in PanelMouseHook's kPanelDismiss post - see above.
    PanelState s;
    CurrentPanelState(s);
    InvalidateRect(h,nullptr,FALSE);
    // While the panel is open the tray icon can afford the same cadence; closed, it falls
    // back to the 30 s Check() tick.
    if(iconDesign==kDescentDesign&&enabled&&!dimmed){
        LASTINPUTINFO input={sizeof(input),0};
        DWORD idle=GetLastInputInfo(&input)?GetTickCount()-input.dwTime:0;
        int step=DescentBucket(idle,idleMs);
        if(iconLiveValid&&iconLiveStep!=step){
            HICON next=LoadStateIcon(kDescentDesign,taskbarLight,TrayIconSize(),kDescentNames[step]);
            if(next){
                if(iconLive)DestroyIcon(iconLive);
                iconLive=next;iconLiveStep=step;
                tray.uFlags=NIF_ICON;Shell_NotifyIconW(NIM_MODIFY,&tray);
            }
        }
    }
}
static LRESULT CALLBACK PanelProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    // Wait for the moving window to settle before interpreting control hits.
    // Tray and click-away dismissal stay available during the entrance.
    if((panelExiting||panelOpening||(panelEntering&&nativebackdrop::Running()))&&
       (msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_MOUSEMOVE))return 0;
    switch(msg){
    case nativebackdrop::kAnimationUpdateMessage: {
        const unsigned session=(unsigned)(wp>>1);
        const bool exiting=(wp&1)!=0;
        if(!nativebackdrop::Running()||session!=nativebackdrop::Session()||!IsWindowVisible(h))return 0;
        if((exiting&&!panelExiting)||(!exiting&&(!panelEntering||panelExiting)))return 0;
        const int y=(int)(LONG_PTR)lp;
        SetWindowPos(h,nullptr,panelTarget.x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        GetWindowRect(h,&panelBounds);
        // Submit content for the applied position now, including the final move. Do not
        // leave a stale composition presentation waiting for the visible-only seconds tick.
        InvalidateRect(h,nullptr,FALSE);
        UpdateWindow(h);
        return 0;
    }
    case nativebackdrop::kAnimationCompleteMessage:
        if(nativebackdrop::Running()&&(unsigned)lp==nativebackdrop::Session()){
            if(wp&&panelExiting)HidePanel(false);
            else if(!wp&&panelEntering&&!panelExiting&&IsWindowVisible(h)){
                // Final position was applied by the preceding update from the same sender.
                // Completion must not introduce a second position correction.
                GetWindowRect(h,&panelBounds);
                panelEntering=false;
                InvalidateRect(h,nullptr,FALSE);
                UpdateWindow(h);
            }
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if(nativebackdrop::Running()&&IsWindowVisible(h))GetWindowRect(h,&panelBounds);
        break;
    case nativebackdrop::kFailureMessage:
        if(panelExiting){HidePanel(false);return 0;}
        panelEntering=false;
        nativebackdrop::Stop();
        InvalidateRect(h,nullptr,FALSE); // opaque fallback; reopen retries the native device
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(h,&ps);
        RECT rc;GetClientRect(h,&rc);
        const int dpi=panelDpi(h);
        PanelState s;CurrentPanelState(s);
        if(nativebackdrop::Running()){
            HBITMAP overlay=PaintPanelOverlay(rc.right,rc.bottom,s,dpi);
            DIBSECTION dib={};
            if(overlay&&GetObject(overlay,sizeof(dib),&dib))
                nativebackdrop::Update(dib.dsBm.bmBits,rc.right,rc.bottom);
            if(overlay)DeleteObject(overlay);
            EndPaint(h,&ps);
            return 0;
        }
        // The live acrylic frame is the panel's base when one has been produced. Lock/Unlock
        // brackets the draw so a concurrent Stop() cannot free the bitmap mid-paint; the lock is
        // held only for this paint, and the worker holds it only long enough to swap an index.
        HBITMAP backdrop=acrylic::LockFrame();
        if(backdrop)acrylicPaints++;
        PaintPanel(dc,rc.right,rc.bottom,s,dpi,backdrop);
        acrylic::UnlockFrame();
        EndPaint(h,&ps);
        return 0;
    }
    case WM_TIMER:
        if(wp==kPanelMotionTimer){PanelMotionTick(h);return 0;}
        if(wp==kPanelTimer){PanelTick(h);return 0;}
        if(wp==kCaptureWatchTimer){
            // A capture tool just took the foreground: freeze now, silently, so its overlay does not
            // hit the "protected app" refusal. After this the panel keeps its frame and is capturable.
            if(acrylic::Running()&&ForegroundIsCaptureTool()){
                KillTimer(panelWindow,kCaptureWatchTimer);
                acrylic::Pause();
                InvalidateRect(panelWindow,nullptr,FALSE);
                Log(L"Acrylic frozen: a screen capture tool took the foreground");
            }
            return 0;
        }
        break;
    case acrylic::kFrameMessage:
        // A new blurred frame is ready (posted by the capture worker, not a timer): repaint with
        // it, keeping the capture rect aligned with wherever the panel currently is.
        {
            RECT pr;if(GetWindowRect(panelWindow,&pr))acrylic::SetCaptureRect(pr);
            InvalidateRect(panelWindow,nullptr,FALSE);
        }
        return 0;
    case kAcrylicPause:
        // Freeze the material where it is and drop the screenshot exclusion. The panel keeps the
        // last frame, so what gets screenshotted is exactly what was on screen.
        acrylic::Pause();
        InvalidateRect(panelWindow,nullptr,FALSE);
        return 0;
    case kPanelShot:
        if(nativebackdrop::Running()){WriteRuntimeReport();return 0;}
        // --panel-shot: render the panel exactly as WM_PAINT does, but into a bitmap we own, and
        // write it to panel-shot.bmp. It cannot be a screen capture: the panel excludes itself from
        // capture (that is what lets it read what is behind itself), so a screenshot would show the
        // desktop instead of the panel. If no session is live it starts one for the shot, so the
        // artifact is deterministic whether or not the panel happens to be open.
        {
            // Geometry must not depend on whether the panel happens to be visible: a hidden window
            // reports DPI 96, which produced a small, wrong-looking render. Use the DPI the panel was
            // last laid out at.
            int dpi=panelDpi(panelWindow);
            if(dpi<=96&&panelEntranceDpi>96)dpi=panelEntranceDpi;
            PanelState probeState;CurrentPanelState(probeState);
            const int bc=probeState.buttonCount>0?probeState.buttonCount:6;
            const int w=MulDiv(kPanelWidth,dpi,96);
            const int h=MulDiv(PanelHeight(bc),dpi,96);
            RECT where;GetWindowRect(panelWindow,&where);
            bool started=false;
            if(!acrylic::Running()&&acrylicEnabled&&w>0&&h>0){
                const bool dark=!taskbarLight;
                const COLORREF tint=dark?RGB(0x20,0x20,0x20):RGB(0xFC,0xFC,0xFC);
                RECT target={where.left,where.top,where.left+w,where.top+h};
                started=acrylic::Start(panelWindow,target,w,h,tint,dark?38:0);
                if(started)Sleep(400);   // audit command: block for one frame rather than race it
            }
            HDC screen=GetDC(nullptr);
            HDC mem=CreateCompatibleDC(screen);
            BITMAPINFO bi={};
            bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth=w;
            bi.bmiHeader.biHeight=-h;
            bi.bmiHeader.biPlanes=1;
            bi.bmiHeader.biBitCount=32;
            bi.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;
            HBITMAP bmp=CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
            if(bmp&&bits){
                SelectObject(mem,bmp);
                PanelState s;CurrentPanelState(s);
                HBITMAP bg=acrylic::LockFrame();
                if(!bg){
                    // The audit command may land before the worker's first frame; give it one
                    // interval rather than silently rendering the flat plate.
                    acrylic::UnlockFrame();
                    Sleep(350);
                    bg=acrylic::LockFrame();
                }
                PaintPanel(mem,w,h,s,dpi,bg);
                acrylic::UnlockFrame();
                wchar_t path[MAX_PATH];
                swprintf(path,MAX_PATH,L"%ls\\panel-shot.bmp",folder);
                acrylic::SaveBitsToBmp(bits,w,h,path);
                SelectObject(mem,nullptr);
                DeleteObject(bmp);
            }
            DeleteDC(mem);
            ReleaseDC(nullptr,screen);
            if(started)acrylic::Stop();
        }
        return 0;
    // ---- mouse ----------------------------------------------------------------
    // Controls are driven directly rather than via focus, so hit testing works regardless of
    // which window holds the caret - the panel is WS_EX_NOACTIVATE and never takes it.
    case WM_LBUTTONDOWN: {
        PanelState s;CurrentPanelState(s);
        int bc=s.buttonCount>0?s.buttonCount:4;
        int hit=HitTest((int)(short)lp,(int)(short)(lp>>16),panelDpi(h),bc);
        panelPressed=hit;
        // The press target may be the caption (which belongs to the tile for clicks), but the
        // highlight follows the tile body only - see HitTestHover.
        panelHover=HitTestHover((int)(short)lp,(int)(short)(lp>>16),panelDpi(h),bc);
        // Capture only for the slider: a drag must keep tracking past the track edge.
        if(hit==hitSliderTrack){
            panelDragging=true;
            SetCapture(h);
            int dpi=panelDpi(h);
            int p=SliderPermilleAtX((int)(short)lp,dpi);
            ApplyThreshold(SliderMagnetBegin(&panelSliderMagnet,p));
        }
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x=(int)(short)lp, y=(int)(short)(lp>>16);
        PanelState s;CurrentPanelState(s);
        int bc=s.buttonCount>0?s.buttonCount:4;
        // Hover uses the stricter geometry: the caption stays clickable, but the highlight is drawn
        // on the box, so pointing at the label must not light it.
        int hit=HitTestHover(x,y,panelDpi(h),bc);
        bool needsRepaint=false;
        if(panelDragging){
            int dpi=panelDpi(h);
            int p=SliderPermilleAtX(x,dpi);
            int m=SliderMagnetUpdate(&panelSliderMagnet,p,
                                     SliderReleasePermille(SliderTrackRect(dpi).w));
            DWORD oldIdle=idleMs;
            ApplyThreshold(m);
            if(idleMs!=oldIdle) needsRepaint=true;
            hit=hitSliderTrack;
        }
        if(hit!=panelHover){
            panelHover=hit;
            needsRepaint=true;
        }
        if(needsRepaint){
            // repaint all, not just the changed control: the press and hover states overlap
            // and a partial invalidation leaves stale edges behind.
            InvalidateRect(h,nullptr,FALSE);
        }
        TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,(HWND)h,0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_LBUTTONUP: {
        PanelState s;CurrentPanelState(s);
        int bc=s.buttonCount>0?s.buttonCount:4;
        int hit=HitTest((int)(short)lp,(int)(short)(lp>>16),panelDpi(h),bc);
        if(panelDragging){
            panelDragging=false;
            ReleaseCapture();
            // Commit once, on release, so a drag does not write settings.ini 200 times.
            SaveSettings();
            if(enabled)Check();
        }else if(hit==panelPressed&&hit!=hitNone){
            ActivatePanelTarget(hit);
        }
        panelPressed=hitNone;
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        panelHover=hitNone;
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        // Losing capture mid-drag (another window took it) must not leave state stuck.
        panelDragging=false;
        panelSliderMagnet.preset=0;
        panelPressed=hitNone;
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;   // fully repainted in WM_PAINT; flicker otherwise
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE){DismissPanel();return 0;}
        break;
    case WM_ACTIVATE:
        // Deliberately empty. Focus-based dismissal was tried and measured failing: the panel
        // is WS_EX_NOACTIVATE, so it never activates and never deactivates. Forcing activation
        // made it worse - the shell reclaimed foreground a second later and the panel hid
        // itself. kPanelDismiss, fed by the WH_MOUSE_LL hook, is the real click-away path.
        break;
    case kPanelDismiss:
        DismissPanel();
        return 0;
    case WM_NCHITTEST: {
        // Outside the panel's rounded rect the click belongs to whatever is underneath.
        LRESULT hit=DefWindowProcW(h,msg,wp,lp);
        POINT p={(short)(lp&0xFFFF),(short)((lp>>16)&0xFFFF)};
        ScreenToClient(h,&p);
        RECT rc;GetClientRect(h,&rc);
        if(p.x<0||p.y<0||p.x>=rc.right||p.y>=rc.bottom)return HTTRANSPARENT;
        return hit;
    }
    }
    return DefWindowProcW(h,msg,wp,lp);
}
// The tray click selects the monitor; placement is fixed at its usable bottom-right corner.
static HWND TaskbarForMonitor(HMONITOR monitor) {
    HWND primary=FindWindowW(L"Shell_TrayWnd",nullptr);
    if(primary&&MonitorFromWindow(primary,MONITOR_DEFAULTTONULL)==monitor){
        return primary;
    }
    HWND secondary=nullptr;
    while((secondary=FindWindowExW(nullptr,secondary,L"Shell_SecondaryTrayWnd",nullptr))!=nullptr){
        if(MonitorFromWindow(secondary,MONITOR_DEFAULTTONULL)==monitor){
            return secondary;
        }
    }
    return nullptr;
}
static void ShowPanel() {
    if(!panelWindow||panelOpening||panelEntering||panelExiting)return;
    panelOpening=true; // before shell/Win32 calls that may reenter the tray callback
    POINT cursor;GetCursorPos(&cursor);
    RECT work={};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    HMONITOR monitor=MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST);
    MONITORINFO info={sizeof(info)};
    if(GetMonitorInfoW(monitor,&info))work=info.rcWork;
    // EarTrumpet reserves the auto-hidden taskbar's full bounds too. rcWork alone includes
    // that space, and GetWindowRect sees only the hidden strip. ABM_GETTASKBARPOS sees the
    // full taskbar even while hidden. Read taskbar geometry once on open.
    APPBARDATA taskbar={sizeof(taskbar)};
    if((SHAppBarMessage(ABM_GETSTATE,&taskbar)&ABS_AUTOHIDE) &&
       SHAppBarMessage(ABM_GETTASKBARPOS,&taskbar) &&
       MonitorFromRect(&taskbar.rc,MONITOR_DEFAULTTONEAREST)==monitor){
        Rect usable=ReserveTaskbarSpace(Rect::FromLTRB(work.left,work.top,work.right,work.bottom),
            Rect::FromLTRB(taskbar.rc.left,taskbar.rc.top,taskbar.rc.right,taskbar.rc.bottom),taskbar.uEdge);
        work=RECT{usable.x,usable.y,usable.x+usable.w,usable.y+usable.h};
    }
    int dpi=96;
    typedef UINT(WINAPI* DpiFn)(HWND);
    DpiFn dpiFor=(DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
    if(dpiFor)dpi=(int)dpiFor(window);
    PanelState s;CurrentPanelState(s);
    int bc=s.buttonCount>0?s.buttonCount:4;
    Rect r=AnchorPanel(dpi,work.left,work.top,
                       work.right-work.left,work.bottom-work.top,bc);
    // No SetForegroundWindow here. A tray app cannot win that fight: the shell reclaims
    // foreground the moment our callback returns, and the losing WA_INACTIVE hides the panel
    // again. The panel stays WS_EX_NOACTIVATE (never steals the caret from whatever the user
    // is typing in, and still receives all mouse input), and dismissal comes from the
    // low-level mouse hook instead.
    BOOL animations=FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
    KillTimer(panelWindow,kPanelMotionTimer);
    panelTarget=r;
    panelEntranceDpi=dpi;

    panelEntering=false;
    panelExiting=false;
    int y=r.y;
    // Prepare native controls/material while cloaked at the final geometry. Only the
    // compatibility renderer below captures the desktop and uses capture exclusion.
    bool nativeStarted=false;
    const bool materialAllowed=acrylic::ShouldStart(acrylicEnabled,true)&&nativebackdrop::Allowed();
    if(materialAllowed){
        // The native target gets its final dimensions before any composition objects are attached.
        // This does not activate or show the window and never moves the user's pointer.
        SetWindowPos(panelWindow,nullptr,r.x,y,r.w,r.h,SWP_NOZORDER|SWP_NOACTIVATE);
        HBITMAP overlay=PaintPanelOverlay(r.w,r.h,s,dpi);
        DIBSECTION dib={};
        if(overlay&&GetObject(overlay,sizeof(dib),&dib))
            nativeStarted=nativebackdrop::Start(panelWindow,r.w,r.h,!taskbarLight,dpi,dib.dsBm.bmBits,animations!=FALSE);
        if(overlay)DeleteObject(overlay);
    }
    if(materialAllowed&&!nativeStarted){
        const bool dark=!taskbarLight;
        const COLORREF tint=dark?RGB(0x20,0x20,0x20):RGB(0xFC,0xFC,0xFC);
        const RECT capture={r.x,r.y,r.x+r.w,r.y+r.h};
        // Tint alpha: Windows' acrylic keeps the backdrop's colour and brightness (its
        // TintLuminosityOpacity layer), so a heavy flat tint is the wrong model - at 200/255 the
        // panel read as dark grey over the video while Windows showed the video's colours through it.
        if(acrylicLiveEnabled)acrylic::Start(panelWindow,capture,r.w,r.h,tint,dark?38:0);
        else acrylic::StartSnapshot(panelWindow,capture,r.w,r.h,tint,dark?38:0);
    }
    panelEntering=animations!=FALSE;
    if(!nativeStarted&&panelEntering){
        // Preparation can take longer than the entrance. Start its clock only when ready to show.
        panelEntranceStart=GetTickCount64();
        if(!SetTimer(panelWindow,kPanelMotionTimer,16,nullptr))panelEntering=false;
        if(panelEntering)y+=PanelEntranceOffset(0,dpi);
    }
    else if(nativeStarted&&panelEntering)y+=MulDiv(nativebackdrop::kNativeSlideTravel,dpi,96);
    HWND taskbarWnd=TaskbarForMonitor(monitor);
    HWND insertAfter=taskbarWnd?taskbarWnd:HWND_TOPMOST;
    if(!SetWindowPos(panelWindow,insertAfter,r.x,y,r.w,r.h,SWP_SHOWWINDOW|SWP_NOACTIVATE)){
        HidePanel(false); // never retain a prepared worker behind a window that failed to show
        return;
    }
    panelBounds=RECT{r.x,y,r.x+r.w,y+r.h};   // hook tests the visible position during the slide
    if(nativeStarted&&panelEntering&&!nativebackdrop::AnimateShow()){
        panelEntering=false;
        nativebackdrop::Stop(); // an animation failure must not leave an invisible interactive panel
        SetWindowPos(panelWindow,nullptr,r.x,r.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        GetWindowRect(panelWindow,&panelBounds);
    }
    else if(nativeStarted&&!panelEntering&&!nativebackdrop::Reveal())nativebackdrop::Stop();
    if(!panelHook) panelHook=SetWindowsHookExW(WH_MOUSE_LL,PanelMouseHook,GetModuleHandleW(nullptr),0);
    SetTimer(panelWindow,kPanelTimer,1000,nullptr);
    // While the material is live the panel is invisible to capture tools, so watch for one taking
    // the foreground and freeze ourselves before it complains. 250 ms is enough: the tools spend
    // longer than that initialising their overlay.
    if(acrylic::Running())SetTimer(panelWindow,kCaptureWatchTimer,250,nullptr);
    // Snapshot mode deliberately does NOT refresh after opening. A refresh repaints only when the
    // capture differs, and when it does the panel visibly changes a moment after appearing - read as
    // the blur "loading". One frame at open is what a still material can promise; use
    // AcrylicLive=1 if the backdrop must track what happens behind the panel.
    InvalidateRect(panelWindow,nullptr,FALSE);
    panelOpening=false; // only preparation is guarded; the visible entrance can be dismissed
}
static void EnsurePanel() {
    if(panelWindow)return;
    WNDCLASSW wc={};
    wc.lpfnWndProc=PanelProc;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=kPanelClass;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    panelWindow=CreateWindowExW(
        // WS_EX_NOACTIVATE is deliberate and required. It keeps the panel out of the activation
        // order (no caret theft while the user is typing) and, critically, it stops the shell
        // from reclaiming foreground and deactivating us - which would make the panel hide itself
        // a second after opening. It still receives every mouse message, so hit testing and the
        // slider drag are unaffected. Click-away is the WH_MOUSE_LL hook, not activation.
        // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab.
        WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,kPanelClass,L"Idle Dimmer",
        WS_POPUP,0,0,MulDiv(kPanelWidth,96,96),MulDiv(PanelHeight(),96,96),
        nullptr,nullptr,wc.hInstance,nullptr);
    if(panelWindow)StylePanel(panelWindow);
}
static void Shutdown();
static void TogglePanel();

// The last accepted tray gesture, and when. Only advanced on an open/close request:
// an ignored balloon or hover delivery arriving between the two halves of one click must
// not reset the pairing window in ResolveTrayGesture, or the second half toggles again and
// the panel flickers. Read here by case kTray: only.
static UINT lastToggleCode=0;
static ULONGLONG lastToggleAt=0;
static bool trayPressStartedOpen=false;
static void ForgetLastToggle(){lastToggleCode=0;lastToggleAt=0;}

// The one way into the UI. Left click, right click, keyboard select and --menu all land here,
// so there is a single answer to "what does clicking the tray icon do" and it cannot be
// answered two different ways by two different code paths. Toggles rather than only shows,
// matching the OS Quick Settings contract: the same gesture closes what it opened.

static void TogglePanel() {
    EnsurePanel();
    if(panelExiting||panelOpening)return; // do not reenter preparation or the current close
    if(IsWindowVisible(panelWindow)||panelEntering)DismissPanel(); else ShowPanel();
}
static void LoadIcons() {
    bool light=TaskbarIsLight();int px=TrayIconSize();
    DropDescentIcon();   // the cached live frame is baked for the old theme variant
    // Design 5 ships off/dim plus six descent frames and has no 'on': while enabled and
    // undimmed it is always showing some descent step, so 'on' would never be displayed.
    bool sunset=iconDesign==kDescentDesign;
    HICON on=sunset?nullptr:LoadStateIcon(iconDesign,light,px,L"on");
    HICON off=LoadStateIcon(iconDesign,light,px,L"off");
    HICON dim=LoadStateIcon(iconDesign,light,px,L"dim");
    if(off&&dim&&(sunset||on)){
        if(iconOn)DestroyIcon(iconOn);
        if(iconOff)DestroyIcon(iconOff);
        if(iconDim)DestroyIcon(iconDim);
        iconOn=on;iconOff=off;iconDim=dim;
    }else{
        if(on)DestroyIcon(on);if(off)DestroyIcon(off);if(dim)DestroyIcon(dim);
        if(!iconOn){ //Artwork not generated yet: keep working with procedural icons.
            iconOn=MakeIcon(RGB(255,194,71),false);
            iconOff=MakeIcon(RGB(150,156,165),false);
            iconDim=MakeIcon(RGB(112,182,255),true);
        }
    }
    taskbarLight=light;
}
// Icon-only menu preview: the theme-matching icon variant composited over the menu color (straight
// alpha, measured with a probe). The active design gets a box outline. DrawIconEx misrenders alpha
// icons on this path, so preview pixels come from GetDIBits directly.
static HBITMAP IconPreview(int design,bool picked) {
    const int iconPx=TrayIconSize(),box=iconPx+8,off=4;
    HDC screen=GetDC(nullptr);if(!screen)return nullptr;
    BITMAPV5HEADER header={};header.bV5Size=sizeof(header);header.bV5Width=box;header.bV5Height=-box;header.bV5Planes=1;header.bV5BitCount=32;header.bV5Compression=BI_BITFIELDS;
    header.bV5RedMask=0x00ff0000;header.bV5GreenMask=0x0000ff00;header.bV5BlueMask=0x000000ff;header.bV5AlphaMask=0xff000000;
    void* pixels=nullptr;HBITMAP dib=CreateDIBSection(screen,(BITMAPINFO*)&header,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dib){ReleaseDC(nullptr,screen);return nullptr;}
    COLORREF menu=GetSysColor(COLOR_MENU);
    DWORD bg=0xff000000|(GetRValue(menu)<<16)|(GetGValue(menu)<<8)|GetBValue(menu);
    DWORD* p=(DWORD*)pixels;for(int i=0;i<box*box;i++)p[i]=bg;
    // Match the icon variant to the actual menu background so glyphs stay visible.
    bool menuLight=(GetRValue(menu)*299u+GetGValue(menu)*587u+GetBValue(menu)*114u)>=128000u;
    HICON icon=LoadStateIcon(design,menuLight,iconPx,design==kDescentDesign?kDescentPreview:L"on");
    if(!icon)icon=MakeIcon(RGB(255,194,71),false);
    if(icon){
        ICONINFO info={};
        if(GetIconInfo(icon,&info)){
            BITMAP bm={};
            if(GetObjectW(info.hbmColor,sizeof(bm),&bm)&&bm.bmBitsPixel==32&&bm.bmWidth>0&&bm.bmHeight>0){
                int iw=bm.bmWidth,ih=bm.bmHeight;
                BYTE* bits=(BYTE*)HeapAlloc(GetProcessHeap(),0,(SIZE_T)iw*ih*4);
                if(bits){
                    BITMAPINFO bi={};bi.bmiHeader.biSize=sizeof(bi.bmiHeader);bi.bmiHeader.biWidth=iw;bi.bmiHeader.biHeight=-ih;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
                    if(GetDIBits(screen,info.hbmColor,0,ih,bits,&bi,DIB_RGB_COLORS)){
                        int x0=off,y0=off;
                        for(int y=0;y<ih;y++){
                            int dy=y0+y;if(dy<0||dy>=box)continue;
                            for(int x=0;x<iw;x++){
                                int dx=x0+x;if(dx<0||dx>=box)continue;
                                BYTE* s=bits+(((SIZE_T)y*iw+x)*4);
                                int a=s[3];if(!a)continue;
                                DWORD t=p[dy*box+dx];int tr=(t>>16)&255,tg=(t>>8)&255,tb=t&255;
                                int r=(s[2]*a+tr*(255-a))/255,g=(s[1]*a+tg*(255-a))/255,b=(s[0]*a+tb*(255-a))/255;
                                p[dy*box+dx]=0xff000000|(r<<16)|(g<<8)|b;
                            }
                        }
                    }
                    HeapFree(GetProcessHeap(),0,bits);
                }
            }
            if(info.hbmColor)DeleteObject(info.hbmColor);
            if(info.hbmMask)DeleteObject(info.hbmMask);
        }
        DestroyIcon(icon);
    }
    if(picked){
        COLORREF mark=RGB((SystemAccent()>>16)&0xFF,(SystemAccent()>>8)&0xFF,SystemAccent()&0xFF);
        DWORD b=0xff000000|(GetRValue(mark)<<16)|(GetGValue(mark)<<8)|GetBValue(mark);
        for(int i=0;i<2;i++)for(int j=0;j<box;j++){p[i*box+j]=b;p[(box-1-i)*box+j]=b;p[j*box+i]=b;p[j*box+box-1-i]=b;}
    }
    ReleaseDC(nullptr,screen);
    return dib;
}
static void AddTray() {
    tray={};tray.cbSize=sizeof(tray);tray.hWnd=window;tray.uID=1;tray.uCallbackMessage=kTray;tray.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE;tray.hIcon=iconOn;
    wcscpy(tray.szTip,L"Idle Dimmer");Shell_NotifyIconW(NIM_ADD,&tray);tray.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray);UpdateTray();
}
static void Shutdown() {
    if(closing)return;
    if(dimmed&&lastStatsTick){sessionstats::AccumulateDimmed(GetTickCount64()-lastStatsTick);lastStatsTick=0;}
    if(!Restore()){
        SaveStats();
        Balloon(L"Brightness could not be restored",L"The app will keep retrying. Use your brightness keys if the display is disconnected.");
        enabled=false;SetThreadExecutionState(ES_CONTINUOUS);KillTimer(window,kCheckTimer);return;
    }
    closing=true;HidePanel();KillTimer(window,kCheckTimer);KillTimer(window,kTestTimer);InputEvents(false);SetThreadExecutionState(ES_CONTINUOUS);
    WTSUnRegisterSessionNotification(window);Shell_NotifyIconW(NIM_DELETE,&tray);DestroyWindow(window);
}
static LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==taskbarCreated){HidePanel();AddTray();return 0;}
    switch(message){
        case kTray: {
            // ONE UI. Every gesture opens the same Quick Settings panel: left-click,
            // right-click, double-click, keyboard select and --menu.
            //
            // Right-click used to raise a native popup offering 1/2/5/10/15 minute presets,
            // an "Exit" row and a "Custom..." row that shelled out to a settings dialog.
            // That was a second front end: two places to implement every feature, and they
            // drifted. It also covered nothing the panel lacks - the panel's slider carries
            // all five presets plus 60, its footer carries Exit, its gear opens the app
            // folder. Having both was the "unbearable UX": a tray icon that behaved
            // differently depending on which button you pressed.
            UINT click=LOWORD(lp);
            if(click==WM_LBUTTONDOWN||click==WM_RBUTTONDOWN||click==WM_LBUTTONDBLCLK){
                trayPressStartedOpen=panelWindow&&IsWindowVisible(panelWindow);
                return 0;
            }
            // Under VERSION_4 that lParam is MAKELPARAM(notificationCode, iconId); wParam
            // carries the cursor anchor via GET_X_LPARAM/GET_Y_LPARAM. Nothing here reads it -
            // the pointer selects the monitor instead - but the split is why LOWORD alone
            // decides anything.
            //
            // The old 300 ms timestamp dedup is gone. It was a guess about a race, and it
            // demonstrably failed: a burst of real clicks produced 19 open/close pairs in 5 s
            // with 0-81 ms between the two TogglePanel() calls of a pair, because the two codes
            // that form one click are different and reset each other's window.
            //
            // One left click is delivered as 513 + 514 + 1024, and 514 and 1024 are both
            // gestures, so classifying each delivery on its own toggled twice per click.
            // ResolveTrayGesture collapses the activation half into the click half; the
            // pairing lives in TrayGesture.h with the measurement that justifies it.
            ULONGLONG now=GetTickCount64();
            DWORD since=lastToggleCode?(DWORD)(now-lastToggleAt):0xFFFFFFFFu;
            UINT openState=(panelWindow&&(IsWindowVisible(panelWindow)||panelOpening||panelEntering))?1u:0u;
            TrayAction act=ResolveTrayGesture(click,lastToggleCode,since,openState);
            if(act==trayToggle){
                lastToggleCode=click;lastToggleAt=now;
                if(TrayPressClosesPanel(click,trayPressStartedOpen))DismissPanel();
                else TogglePanel();
                if(click==WM_LBUTTONUP||click==WM_CONTEXTMENU)trayPressStartedOpen=false;
                // Closing clears old pairing state. Record this accepted gesture afterwards,
                // and retain it across the final hide so its NIN_SELECT cannot reopen us.
                lastToggleCode=click;lastToggleAt=now;
            }
            return 0;
        }
    case WM_TIMER:
        if(wp==kDisplayOffDelayTimer){ApplyDisplayOff();return 0;}
        if(wp==kCheckTimer)Check();
        if(wp==kOffTimer){
            // One-shot: the second stage of a dim. The timer is re-armed by the next Dim(), and
            // Restore() kills it, so a screen that woke early is never switched off afterwards.
            KillTimer(window,kOffTimer);
            if(dimmed)OffDisplays();
        }
        if(wp==kTestTimer){
            KillTimer(window,kTestTimer);Saved low={},after={};DdcSaved dLow={},dAfter={};
            HRESULT readLow=Capture(low);CaptureDdc(dLow);testing=false;
            bool restored=Restore();HRESULT readAfter=Capture(after);CaptureDdc(dAfter);graceUntil=GetTickCount64()+idleMs;
            // Either mechanism may have done the dimming, and each is verified on its own: WMI
            // against the internal-panel percentages, DDC against the external monitors' levels.
            // The test used to know only about WMI, so on a desktop with no internal panel it
            // reported FAIL no matter what the screen actually did.
            bool wmiTested=testBefore.count>0;
            bool wmiPass=wmiTested&&SUCCEEDED(readLow)&&SUCCEEDED(readAfter)&&low.count==testBefore.count&&after.count==testBefore.count;
            for(DWORD i=0;i<testBefore.count&&wmiPass;i++)wmiPass=low.panels[i].brightness==0&&after.panels[i].brightness==testBefore.panels[i].brightness;
            bool ddcTested=testBeforeDdc.count>0;
            bool ddcPass=ddcTested&&dLow.count==testBeforeDdc.count&&dAfter.count==testBeforeDdc.count;
            for(DWORD i=0;i<testBeforeDdc.count&&ddcPass;i++){
                DWORD lowValue=dLow.monitors[i].brightness;
                DWORD backValue=dAfter.monitors[i].brightness;
                DWORD before=testBeforeDdc.monitors[i].brightness;
                // At the floor, and back to where it started. One step of slack: DDC levels are
                // integers a monitor may round, and a stuck or ignored write is many steps away.
                ddcPass=(lowValue<=dLow.monitors[i].min)&&(backValue+1>=before&&backValue<=before+1);
            }
            bool passed=restored&&(wmiTested||ddcTested)&&(!wmiTested||wmiPass)&&(!ddcTested||ddcPass);
            FILE* f=_wfopen(reportFile,L"w, ccs=UTF-8");
            if(f){
                fwprintf(f,L"Hardware dim/restore test: %ls\nWMI panels: %lu\nBefore: %u%%\nDimmed: %u%%\nRestored: %u%%\nDDC monitors: %lu\n",passed?L"PASS":L"FAIL",testBefore.count,testBefore.panels[0].brightness,low.panels[0].brightness,after.panels[0].brightness,testBeforeDdc.count);
                for(DWORD i=0;i<testBeforeDdc.count;i++)fwprintf(f,L"Monitor %lu: before %lu%% -> dimmed %lu%% -> restored %lu%%\n",i+1,testBeforeDdc.monitors[i].brightness,dLow.monitors[i].brightness,dAfter.monitors[i].brightness);
                fclose(f);
            }
            if(testExit)Shutdown();
        }
        return 0;
    case kDisplayOffApply:
        if((UINT)wp==displayOff.generation)ApplyDisplayOff();
        return 0;
    case WM_INPUT: {
        bool meaningful=false;
        HRAWINPUT hRaw=(HRAWINPUT)lp;UINT size=0;
        if(GetRawInputData(hRaw,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))==0&&
           size>=sizeof(RAWINPUTHEADER)&&size<=512){
            alignas(RAWINPUT) BYTE rawBuf[512];
            if(GetRawInputData(hRaw,RID_INPUT,rawBuf,&size,sizeof(RAWINPUTHEADER))==size){
                const RAWINPUT* raw=(const RAWINPUT*)rawBuf;
                if(raw->header.dwType==RIM_TYPEMOUSE&&size>=sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE))
                    meaningful=displaypower::HasMouseAction(raw->data.mouse.lLastX,raw->data.mouse.lLastY,raw->data.mouse.usButtonFlags);
                else if(raw->header.dwType==RIM_TYPEKEYBOARD&&size>=sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD))meaningful=true;
                else if(raw->header.dwType==RIM_TYPEHID&&size>sizeof(RAWINPUTHEADER))meaningful=true;
            }
        }
        if(size>512){
            // Touch/digitizer reports can be large. The header is sufficient to classify
            // them; do not allocate an unbounded payload or silently lose touch wake.
            RAWINPUTHEADER header={};UINT headerSize=sizeof(header);
            if(GetRawInputData(hRaw,RID_HEADER,&header,&headerSize,sizeof(header))==sizeof(header)&&
               header.dwType==RIM_TYPEHID)meaningful=true;
        }
        const DWORD eventTick=(DWORD)GetMessageTime();
        if(displayOff.Active()&&!displayOff.AllowsWake(eventTick,meaningful)){
            break;
        }
        if(meaningful&&!testing&&!restorePending&&(displayOff.Active()||dimmed)&&
           GetTickCount64()>=manualDimIgnoreUntil){
            // Tag the deferred work. A stale input message must not wake a later off session.
            if(PostMessageW(hwnd,kRestore,displayOff.generation,eventTick)){
                restorePending=true;
            }
        }
        break; // DefWindowProc performs required raw-input cleanup.
    }
    case kRestore:
        if((UINT)wp!=displayOff.generation)return 0;
        restorePending=false;manualDimIgnoreUntil=0ULL;
        Restore();graceUntil=GetTickCount64()+idleMs;ScheduleNext();return 0;
    case kEnable: enabled=true;ApplyEnabled();return 0;
    case kReport: WriteRuntimeReport();return 0;
    case kShowPanel: TogglePanel();return 0;
    case WM_COMMAND:
        // kDimTest is the only command that still arrives as a message: --test-brightness
        // posts it, and the panel's "Dim now" button calls StartDimTest() directly. The
        // kIconFirst branch the old menu needed is gone - the icon picker is a
        // TPM_RETURNCMD popup that applies its own selection, so it never routed here.
        if(LOWORD(wp)==kDimTest)StartDimTest();
        return 0;
    case WM_WTSSESSION_CHANGE:
        if(wp==WTS_SESSION_LOCK){locked=true;Restore(false);SetThreadExecutionState(ES_CONTINUOUS);ScheduleNext();}
        if(wp==WTS_SESSION_UNLOCK){locked=false;graceUntil=GetTickCount64()+idleMs;if(enabled)SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED);ScheduleNext();}
        return 0;
    case WM_POWERBROADCAST:
        if(nativebackdrop::Running()&&!nativebackdrop::Allowed()){
            nativebackdrop::Stop();InvalidateRect(panelWindow,nullptr,FALSE);
        }
        if(wp==PBT_APMSUSPEND){
            Restore(false);SetThreadExecutionState(ES_CONTINUOUS);
            KillTimer(window,kCheckTimer);
        }
        if(wp==PBT_APMRESUMEAUTOMATIC||wp==PBT_APMRESUMESUSPEND){
            Restore(false);
            media.reset();
            mediaOperation.reset();
            graceUntil=GetTickCount64()+idleMs;
            if(wp==PBT_APMRESUMESUSPEND&&enabled&&!locked)SetKeepAwake(true);
            ScheduleNext();
        }
        return TRUE;
    case WM_SETTINGCHANGE:
        if(nativebackdrop::Running()&&!nativebackdrop::Allowed()){
            nativebackdrop::Stop();InvalidateRect(panelWindow,nullptr,FALSE);
        }
        // Documented theme signal: re-read SystemUsesLightTheme and reload only when it actually
        // changed; the message also fires for unrelated settings and on lock/UAC (same guard Windows
        // Terminal ships for its taskbar icon).
        if(lp&&!wcscmp((const wchar_t*)lp,L"ImmersiveColorSet")&&TaskbarIsLight()!=taskbarLight){LoadIcons();UpdateTray();}
        return 0;
    case WM_THEMECHANGED:
        if(TaskbarIsLight()!=taskbarLight){LoadIcons();UpdateTray();}
        break;
    case WM_QUERYENDSESSION: Restore();return TRUE;
    case WM_ENDSESSION: if(wp)Shutdown();return 0;
    case WM_CLOSE: case kQuit: Shutdown();return 0;
    case WM_DESTROY: PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
// Counts assertions so the report cannot drift from what is actually run. Incremented by
// the helpers below as they execute, which is why it is a running total rather than a
// hand-maintained literal that silently goes stale.
static int flyoutChecks=0;
static wchar_t flyoutFirstFailure[256]=L"";
static bool Check(bool condition,const wchar_t* what) {
    flyoutChecks++;
    if(!condition&&!flyoutFirstFailure[0])swprintf(flyoutFirstFailure,256,L"%ls",what);
    return condition;
}
// Assertions already counted above SelfTest(): 12 original dim/clamp/wrap checks plus the 4
// DescentBucket checks appended with the sunset work, plus the 13 tray-gesture classification
// checks, plus the 9 gesture-pairing checks, plus the 4 click-away rect checks, plus the 19
// settings parse/format/heal checks, plus 5 tray-close press/release checks. flyoutChecks is
// added at run time.
static const int kTrayCheckCount=66;

// Layout assertions for the flyout panel, kept out of SelfTest() so the tray decision
// tests stay readable. Pure functions only: no window, no DC, no global state.
static bool FlyoutLayoutSelfTest() {
    flyoutChecks=0;
    bool ok=true;

    // Every slider position resolves to a legal minute. The mapping is per-mille, not
    // percent: at 0..100 adjacent values collapsed (38 and 39 both landed on 89%), which
    // made "all minutes 1..60" impossible no matter how the drag was handled.
    for(int p=0;p<=kSliderPermilleMax;p++){
        int m=MinutesFromPermille(p);
        ok &= Check(m>=1&&m<=60,L"slider per-mille resolves to a legal threshold");
    }
    for(int m=1;m<=60;m++){
        ok &= Check(MinutesFromPermille(PermilleFromMinutes(m))==m,L"every minute round-trips");
        ok &= Check(PermilleFromMinutes(m)>=0&&PermilleFromMinutes(m)<=kSliderPermilleMax,L"minute position in range");
    }
    // 39 must be its own position, not a rounding casualty of 38 or 40.
    ok &= Check(MinutesFromPermille(PermilleFromMinutes(39))==39,L"39 minutes is selectable");
    ok &= Check(PermilleFromMinutes(39)!=PermilleFromMinutes(38),L"39 is distinct from 38");
    ok &= Check(PermilleFromMinutes(39)!=PermilleFromMinutes(40),L"39 is distinct from 40");
    for(int m=2;m<=60;m++){
        ok &= Check(PermilleFromMinutes(m)>PermilleFromMinutes(m-1),L"minute positions strictly increase");
    }
    // Monotonic: dragging right never lowers the threshold. Without this, the snap scan can
    // pick a lower minute for a higher position and the thumb fights the user.
    int previous=0;
    for(int p=0;p<=kSliderPermilleMax;p++){
        int m=MinutesFromPermille(p);
        if(m<previous)ok &= Check(false,L"slider is monotonic");
        previous=m;
    }
    ok &= Check(true,L"monotonicity sweep");

    // The magnet is one-way: approaching a preset pulls it in, moving away one pixel
    // releases. A click never snaps, so values inside a pull-in zone are still clickable.
    {
        SliderMagnet mag={};
        const int release96=SliderReleasePermille(242);
        const int release192=SliderReleasePermille(484);
        const int p30=PermilleFromMinutes(30);
        ok &= Check(SliderMagnetBegin(&mag,PermilleFromMinutes(39))==39,L"click on 39 selects 39");
        ok &= Check(mag.preset==0,L"a click does not magnetise");
        // Start outside the pull-in zone and drag into it, which is the only way a magnet
        // may capture. Positions are derived from the linear table, not written as log
        // constants that stopped meaning anything when the scale changed.
        SliderMagnetBegin(&mag,p30-kSliderSnapEnterPermille+5);
        int v=SliderMagnetUpdate(&mag,p30-7,release96);
        ok &= Check(v==30&&mag.preset==30,L"approaching 30 pulls it in");
        v=SliderMagnetUpdate(&mag,p30-2,release96);
        ok &= Check(v==30,L"continuing toward a preset keeps it");
        SliderMagnetUpdate(&mag,p30-2-release96,release96);
        ok &= Check(mag.preset==0,L"moving away releases after one pixel");
        // Moving away while already inside the zone must not capture the value.
        SliderMagnetBegin(&mag,p30-12);
        SliderMagnetUpdate(&mag,p30-22,release96);
        ok &= Check(mag.preset==0,L"moving away does not magnetise");
        // 39 is far enough from 30 and 60 that neither magnet owns it.
        SliderMagnetBegin(&mag,PermilleFromMinutes(39)-6);
        v=SliderMagnetUpdate(&mag,PermilleFromMinutes(39),release96);
        ok &= Check(v==39&&mag.preset==0,L"39 stays free of the presets");
        // Release scales with DPI: the same one-pixel rule is a smaller per-mille step at
        // 200%, so a move that holds at 96 DPI releases at 192 DPI.
        SliderMagnetBegin(&mag,p30-kSliderSnapEnterPermille+5);
        SliderMagnetUpdate(&mag,p30-7,release192);
        SliderMagnetUpdate(&mag,p30-2,release192);
        SliderMagnetUpdate(&mag,p30-2-(release192-1),release192);
        ok &= Check(mag.preset==30,L"a sub-pixel move at 200 percent still holds");
        SliderMagnetUpdate(&mag,p30-2-release96,release192);
        ok &= Check(mag.preset==0,L"release is one pixel at 200 percent");
    }

    // The power card's progress bar: session dimmed time over process uptime. Pure arithmetic,
    // so the clamp and the rounding are pinned here rather than eyeballed on a render.
    {
        ok &= Check(ProgressFillWidth(100,0,100)==0,L"bar: no dimmed time paints nothing");
        ok &= Check(ProgressFillWidth(100,50,100)==50,L"bar: half the session fills half the track");
        ok &= Check(ProgressFillWidth(100,100,100)==100,L"bar: the whole session fills the track");
        ok &= Check(ProgressFillWidth(100,150,100)==100,L"bar: a part larger than the whole clamps");
        ok &= Check(ProgressFillWidth(100,50,0)==0,L"bar: no uptime yet paints nothing");
        ok &= Check(ProgressFillWidth(0,50,100)==0,L"bar: a zero-width track paints nothing");
        ok &= Check(ProgressFillWidth(100,1,3)==33,L"bar: thirds round down");
        ok &= Check(ProgressFillWidth(100,2,3)==67,L"bar: thirds round up");
        // A week of uptime in milliseconds is past 32 bits; the multiply must not overflow.
        ok &= Check(ProgressFillWidth(100,8000000000000ULL,16000000000000ULL)==50,L"bar: 64-bit spans do not overflow");
        ok &= Check(ProgressFillWidth(97,1,1)==97,L"bar: odd widths still fill exactly");
    }

    // Out-of-range input is clamped, not wrapped or crashed on.
    ok &= Check(MinutesFromPermille(-50)>=1&&MinutesFromPermille(5000)<=60,L"per-mille input clamped");
    ok &= Check(PermilleFromMinutes(0)>=0&&PermilleFromMinutes(9999)<=kSliderPermilleMax,L"minutes input clamped");

    // Panel height is positive and grows with content, never collapses to zero.
    ok &= Check(PanelHeight()>0,L"panel height positive");
    // The panel is a single tile row now (four columns), not the two rows this used to guard
    // against. One row plus its caption band must still fit inside the declared height.
    ok &= Check(PanelHeight()>kTileHeight+kCaptionGap+kCaptionH,L"panel height fits the single tile row");
    // Every element must FIT inside the declared panel height. PanelHeight() and the painter
    // each used to compute the content stack separately; the copies drifted and the divider
    // painted through the second row's captions, so every caption was sliced in half. The
    // symptom is invisible in code and obvious in verify-panel.png, which is why it is gated.
    {
        // 4-tile layout
        Rect last4=TileRect(3,96,4);
        const int capTop4=last4.y+last4.h+kCaptionGap;
        const int capBot4=capTop4+kCaptionH;
        ok &= Check(capBot4<=PanelHeight(4),L"4-tile: last caption fits inside panel height");
        ok &= Check(DividerY(4)>capBot4,L"4-tile: divider clears the last caption");

        // 6-tile reflowed layout
        Rect last6=TileRect(5,96,6);
        const int capTop6=last6.y+last6.h+kCaptionGap;
        const int capBot6=capTop6+kCaptionH;
        ok &= Check(capBot6<=PanelHeight(6),L"6-tile: last caption fits inside panel height");
        ok &= Check(DividerY(6)>capBot6,L"6-tile: divider clears the last caption");

        ok &= Check(SliderCenterY()>DividerY(),L"slider sits below the divider");
        ok &= Check(TickY()>SliderCenterY(),L"tick row sits below the slider");
        ok &= Check(FooterDividerY()>TickY(),L"footer divider sits below the ticks");
        ok &= Check(FooterY()+kFooterHeight<=PanelHeight(),L"footer fits inside panel height");
        // Monotonic: the stack must never go backwards, at any DPI.
        const int d2[2]={96,192};
        for(int q=0;q<2;q++){
            const int d=d2[q];
            const int s=MulDiv(kSliderY,d,96), t=MulDiv(kTickY,d,96);
            ok &= Check(s<t,L"stack is monotonic at this dpi");
        }
    }

    // Hit testing: every control must answer for a point inside it, and nothing outside the
    // panel may resolve to a control. A mis-mapped target either swallows clicks silently or
    // fires the wrong action, and a rendered sheet cannot show either - so it is asserted.
    {
        const int dpis[2]={96,192};
        for(int d=0;d<2;d++){
            const int dpi=dpis[d];
            struct Probe { int target; int cx, cy; };
            Probe probes[12];
            int n=0;
            // The live panel has six tiles; probe exactly those, in the order the model assigns,
            // using totalButtons=6 so the coordinates are the ones the panel actually uses.
            const int tileTargets[6]={hitTileDimming,hitTileMedia,hitTileSound,hitTileFullscreen,hitTileOffMode,hitTileAutostart};
            for(int i=0;i<6;i++){
                Rect t=TileRect(i,dpi,6);
                probes[n].target=tileTargets[i];
                probes[n].cx=t.x+t.w/2;
                probes[n].cy=t.y+t.h/2;
                n++;
            }
            Rect tr=SliderTrackRect(dpi,6);
            probes[n].target=hitSliderTrack; probes[n].cx=tr.x+tr.w/2; probes[n].cy=tr.y; n++;
            Rect dn=DimNowRect(dpi,6);
            probes[n].target=hitDimNow; probes[n].cx=dn.x+dn.w/2; probes[n].cy=dn.y+dn.h/2; n++;
            // The new button must not overlap dim-now or exit: its own centre has to resolve to
            // its own target, which is exactly what an overlap would break.
            Rect off=DisplaysOffRect(dpi,6);
            probes[n].target=hitDisplaysOff; probes[n].cx=off.x+off.w/2; probes[n].cy=off.y+off.h/2; n++;
            Rect fe=FooterExitRect(dpi,6);
            probes[n].target=hitFooterExit; probes[n].cx=fe.x+fe.w/2; probes[n].cy=fe.y+fe.h/2; n++;
            for(int p=0;p<n;p++){
                int got=HitTest(probes[p].cx,probes[p].cy,dpi,6);
                ok &= Check(got==probes[p].target,L"control centre maps to its own target");
                // Every probed point must be inside the panel, otherwise the probe is itself
                // meaningless and the assertion would pass for the wrong reason.
                ok &= Check(probes[p].cx>=0&&probes[p].cx<=MulDiv(kPanelWidth,dpi,96),L"probe point inside panel width");
            }
            // Far outside: nothing resolves.
            ok &= Check(HitTest(-500,-500,dpi)==hitNone,L"point off-panel is not a control");
            ok &= Check(HitTest(MulDiv(kPanelWidth,dpi,96)+500,MulDiv(PanelHeight(),dpi,96)+500,dpi)==hitNone,L"point past panel is not a control");
            // The caption band belongs to its tile for CLICKS. Hover is stricter: the highlight is
            // painted on the box, so a pointer resting on the label must not light the tile. Both
            // come from one implementation with two geometries, so they cannot drift apart.
            {
                Rect t=TileRect(0,dpi);
                int capY=t.y+t.h+MulDiv(kCaptionGap,dpi,96);   // S() is file-local to Flyout.cpp
                ok &= Check(HitTest(t.x+t.w/2,capY,dpi)==hitTileDimming,L"caption area activates its tile");
                ok &= Check(HitTestHover(t.x+t.w/2,capY,dpi)==hitNone,L"caption area does not highlight its tile");
                ok &= Check(HitTestHover(t.x+t.w/2,t.y+t.h/2,dpi)==hitTileDimming,L"tile body highlights its tile");
                ok &= Check(HitTestHover(t.x+2,t.y+t.h/2,dpi)==hitTileDimming,L"tile body left edge still highlights");
                ok &= Check(HitTestHover(t.x+t.w-2,t.y+t.h-2,dpi)==hitTileDimming,L"tile body bottom-right still highlights");
                ok &= Check(HitTestHover(t.x+t.w/2,t.y+t.h+MulDiv(kCaptionGap+kCaptionH+2,dpi,96),dpi)==hitNone,L"below the caption is not a control");
            }
            // Adjacent controls must not overlap: overlapping buttons make the one drawn last
            // win, which is an invisible ordering dependency.
            ok &= Check(!DimNowRect(dpi).Contains(tr.x,tr.y)||tr.x+tr.w<=DimNowRect(dpi).x,L"slider and dim-now do not overlap");
        }
        // The thumb must stay on the track, and every minute 1..60 must round-trip. This is
        // now a real continuous slider: the value is not forced onto one of seven detents.
        {
            Rect track=SliderTrackRect(96);
            for(int i=0;i<DetentCount();i++){
                int m=DetentAt(i);
                int x=SliderThumbX(96,m);
                int back=MinutesFromPermille(SliderPermilleAtX(x,96));
                ok &= Check(x>=track.x&&x<=track.x+track.w,L"thumb inside track");
                ok &= Check(back==m,L"detent round-trips through the thumb position");
            }
            for(int m=1;m<=60;m++){
                int x=SliderThumbX(96,m);
                int back=MinutesFromPermille(SliderPermilleAtX(x,96));
                ok &= Check(back==m,L"every minute round-trips through the thumb position");
                ok &= Check(x>=track.x&&x<=track.x+track.w,L"every minute thumb is inside the track");
            }
            // Every minute must have its own pixel position, not just its own value. If two
            // minutes share a thumb position, the painter and the hit test disagree.
            for(int m=2;m<=60;m++){
                ok &= Check(SliderThumbX(96,m)>SliderThumbX(96,m-1),L"thumb positions strictly increase");
            }
            // Linear scale: every one-minute step must be the same physical width. On the
            // old log scale 1->2 was dozens of px and 59->60 was one or two; that asymmetry
            // is what the user rejected. 242 px / 59 intervals is 4.1, so steps alternate 4
            // and 5 px; 484 px / 59 is 8.2, so 8 and 9. A step outside that band means a
            // second rounding crept back in. No dots are painted, so the old dot coordinates
            // are gone - the position contract is the spacing itself.
            const int dpis2[2]={96,192};
            for(int q=0;q<2;q++){
                const int d=dpis2[q];
                const Rect trk=SliderTrackRect(d);
                const int lo=trk.w/59;
                const int hi=(trk.w+58)/59;
                ok &= Check(SliderThumbX(d,1)==trk.x,L"minute 1 sits at the left end of the track");
                ok &= Check(SliderThumbX(d,60)==trk.x+trk.w,L"minute 60 sits at the right end of the track");
                for(int m=2;m<=60;m++){
                    const int step=SliderThumbX(d,m)-SliderThumbX(d,m-1);
                    ok &= Check(step>=lo&&step<=hi,L"every minute step is an equal share of the track");
                }
            }
            for(int i=1;i<DetentCount();i++){
                ok &= Check(DetentAt(i)>DetentAt(i-1),L"detents strictly increase");
                ok &= Check(SliderThumbX(96,DetentAt(i))>SliderThumbX(96,DetentAt(i-1)),L"detent thumbs strictly increase");
            }
            // The removed context menu offered these six thresholds through kIdleFirst commands, and
            // the panel replaced that menu. Every one must still be reachable from the
            // slider, which means it must still be one of the seven detents.
            for(int i=0;i<6;i++){
                int m=(int)kIdlePresets[i];
                bool isDetent=false;
                for(int j=0;j<DetentCount();j++)if(DetentAt(j)==m)isDetent=true;
                ok &= Check(isDetent,L"every legacy menu idle preset is still a slider detent");
            }
        }
        ok &= Check(SliderPermilleAtX(-9999,96)>=0,L"per-mille clamps below the track");
        ok &= Check(SliderPermilleAtX(99999,96)<=kSliderPermilleMax,L"per-mille clamps past the track");
    }
    // Sweep every pixel of the panel and require it to hit test to a target ActivatePanelTarget
    // actually acts on. A point that resolves to an unhandled target swallows the click
    // silently, and a rendered contact sheet cannot show that - only a sweep can. The upper
    // bound is hitFooterExit, the last enumerator in Flyout.h, which this change does not own.
    {
        const int sweepDpis[2]={96,192};
        for(int d=0;d<2;d++){
            const int dpi=sweepDpis[d];
            bool allKnown=true;
            for(int y=0;y<MulDiv(PanelHeight(),dpi,96);y+=2){
                for(int x=0;x<MulDiv(kPanelWidth,dpi,96);x+=2){
                    int hit=HitTest(x,y,dpi);
                    if(hit<hitNone||hit>hitFooterExit)allKnown=false;
                }
            }
            ok &= Check(allKnown,L"every panel pixel resolves to a known target");
        }
    }
    // Tile grid: four non-overlapping rects inside the panel width, at 100% and 200%.
    const int dpis[2]={96,192};
    for(int d=0;d<2;d++){int dpi=dpis[d];
        int panelW=MulDiv(kPanelWidth,dpi,96);
        for(int i=0;i<4;i++){
            Rect t=TileRect(i,dpi);
            ok &= Check(t.w>0&&t.h>0,L"tile has non-zero size");
            ok &= Check(t.x>=0&&t.x+t.w<=panelW,L"tile inside panel width");
        }
        // Single row: column 0 left of column 1.
        ok &= Check(TileRect(0,dpi).x<TileRect(1,dpi).x,L"tile col 0 left of col 1");
        // Out-of-range indices return an empty rect rather than drawing something stray.
        ok &= Check(TileRect(-1,dpi).w==0&&TileRect(4,dpi).w==0,L"out-of-range tile index empty");
        // The slider track sits below the tile row.
        ok &= Check(SliderTrackRect(dpi).y>=TileRect(3,dpi).y+TileRect(3,dpi).h-1,L"slider below tile row");
    }

    // ---- Redesigned stack: four tiles in ONE row, then the power card and the footer -------
    // The panel used to be a six-tile, two-row grid. It is now four tiles in a single row,
    // followed by the power dashboard card and the footer. These assertions describe that
    // stack directly, so a spacing or column-count change has to face them.
    {
        const int rowDpis[2]={96,192};
        for(int d=0;d<2;d++){int dpi=rowDpis[d];
            const int gap=MulDiv(kTileGap,dpi,96);
            // One row: all four tiles share a top edge, so none can drift into a second row.
            ok &= Check(TileRect(0,dpi).y==TileRect(1,dpi).y&&TileRect(1,dpi).y==TileRect(2,dpi).y&&TileRect(2,dpi).y==TileRect(3,dpi).y,L"all four tiles share one row top");
            for(int i=1;i<4;i++){
                Rect prev=TileRect(i-1,dpi),cur=TileRect(i,dpi);
                // Strictly to the right of the previous tile's right edge: no shared edge
                // pixel, which would let two tiles claim the same point.
                ok &= Check(cur.x>prev.x+prev.w,L"each tile starts right of the previous tile edge");
                // Columns are evenly spaced: exactly one kTileGap between abutting tiles.
                ok &= Check(cur.x-(prev.x+prev.w)==gap,L"between-tile gap is exactly kTileGap");
            }
        }
        // Pairwise non-overlap across all four tiles (six pairs). Deliberately redundant with
        // the ordering checks: a direct disjointness test is what "no overlap" actually means.
        for(int a=0;a<4;a++)for(int b=a+1;b<4;b++){
            Rect ra=TileRect(a,96),rb=TileRect(b,96);
            const bool disjoint=ra.x+ra.w<=rb.x||rb.x+rb.w<=ra.x||ra.y+ra.h<=rb.y||rb.y+rb.h<=ra.y;
            ok &= Check(disjoint,L"tile pairs do not overlap");
        }
        // The caption band under every tile (box, kCaptionGap, kCaptionH) must sit entirely
        // above the divider. The old grid read TileRect(5) as "the last tile"; with four
        // columns that index is now an empty rect, so this pins the band against tiles that
        // actually exist instead of asserting against a zero-height phantom.
        for(int i=0;i<4;i++){
            Rect t=TileRect(i,96);
            const int capBot=t.y+t.h+kCaptionGap+kCaptionH;
            ok &= Check(capBot<=DividerY(),L"tile caption band clears the divider");
        }
    }
    // Dynamic reflow assertions: 5 buttons reflow into 2 balanced rows (3 top, 2 bottom, centered)
    // and 6 buttons reflow into 2 rows (3 top, 3 bottom) with zero orphan buttons.
    {
        // 5 buttons: 3 on row 0, 2 on row 1
        ok &= Check(TileRect(0,96,5).y==TileRect(1,96,5).y&&TileRect(1,96,5).y==TileRect(2,96,5).y,L"5-tile: row 0 tiles share top edge");
        ok &= Check(TileRect(3,96,5).y==TileRect(4,96,5).y,L"5-tile: row 1 tiles share top edge");
        ok &= Check(TileRect(3,96,5).y>TileRect(0,96,5).y,L"5-tile: row 1 is below row 0");
        ok &= Check(TileRect(3,96,5).x>kPanelPadX,L"5-tile: row 1 is inset/centered");
        // Row 1 is centered: left margin equals right margin
        int leftMargin = TileRect(3,96,5).x;
        int rightMargin = kPanelWidth - (TileRect(4,96,5).x + TileRect(4,96,5).w);
        ok &= Check(leftMargin == rightMargin, L"5-tile: row 1 is symmetrically centered with no orphan button");
        // No overlap among all 5 tiles (10 pairs)
        for(int a=0;a<5;a++)for(int b=a+1;b<5;b++){
            Rect ra=TileRect(a,96,5),rb=TileRect(b,96,5);
            bool disjoint=ra.x+ra.w<=rb.x||rb.x+rb.w<=ra.x||ra.y+ra.h<=rb.y||rb.y+rb.h<=ra.y;
            ok &= Check(disjoint,L"5-tile: tile pairs do not overlap");
        }
        ok &= Check(TileRect(5,96,5).w==0,L"5-tile: index 5 is out of range");

        // 6 buttons: 3 on row 0, 3 on row 1
        ok &= Check(TileRect(0,96,6).y==TileRect(1,96,6).y&&TileRect(1,96,6).y==TileRect(2,96,6).y,L"6-tile: row 0 tiles share top edge");
        ok &= Check(TileRect(3,96,6).y==TileRect(4,96,6).y&&TileRect(4,96,6).y==TileRect(5,96,6).y,L"6-tile: row 1 tiles share top edge");
        ok &= Check(TileRect(3,96,6).y>TileRect(0,96,6).y,L"6-tile: row 1 is below row 0");
        ok &= Check(TileRect(0,96,6).x==TileRect(3,96,6).x,L"6-tile: columns align vertically");
        ok &= Check(TileRect(6,96,6).w==0,L"6-tile: index 6 is out of range");

        // Stack expansion with 2 rows: PanelHeight and DividerY expand by kRowPitch
        ok &= Check(DividerY(5)==DividerY(4)+kRowPitch,L"5-tile: divider expands by kRowPitch");
        ok &= Check(PanelHeight(5)==PanelHeight(4)+kRowPitch,L"5-tile: panel height expands by kRowPitch");
        ok &= Check(PanelHeight(6)==PanelHeight(4)+kRowPitch,L"6-tile: panel height expands by kRowPitch");
        // Vertical gap between row 0 caption and row 1 tiles
        ok &= Check(TileRect(3,96,5).y >= TileRect(0,96,5).y + TileRect(0,96,5).h + kCaptionGap + kCaptionH + kRowGap, L"5-tile: row 1 clears row 0 caption with kRowGap");

        // Hit testing and dynamic reflow: all 5 tiles are independently hittable
        ok &= Check(HitTest(TileRect(0,96,5).x+10, TileRect(0,96,5).y+10, 96, 5) == hitTileDimming, L"5-tile: hit tile 0 is Dimming");
        ok &= Check(HitTest(TileRect(1,96,5).x+10, TileRect(1,96,5).y+10, 96, 5) == hitTileMedia, L"5-tile: hit tile 1 is Media");
        ok &= Check(HitTest(TileRect(2,96,5).x+10, TileRect(2,96,5).y+10, 96, 5) == hitTileSound, L"5-tile: hit tile 2 is Sound");
        ok &= Check(HitTest(TileRect(3,96,5).x+10, TileRect(3,96,5).y+10, 96, 5) == hitTileFullscreen, L"5-tile: hit tile 3 is Fullscreen");
        ok &= Check(HitTest(TileRect(4,96,5).x+10, TileRect(4,96,5).y+10, 96, 5) == hitTileOffMode, L"5-tile: hit tile 4 is Off at idle");

        // 6 tiles hit test: the six the panel actually shows
        ok &= Check(HitTest(TileRect(4,96,6).x+10, TileRect(4,96,6).y+10, 96, 6) == hitTileOffMode, L"6-tile: hit tile 4 is Off at idle");
        ok &= Check(HitTest(TileRect(5,96,6).x+10, TileRect(5,96,6).y+10, 96, 6) == hitTileAutostart, L"6-tile: hit tile 5 is Autostart");
        // Every drawn tile must resolve to a target, and no two tiles may share one - a shared
        // target means one of them toggles the other one's setting.
        int seenTargets[6]={};
        for(int i=0;i<6;i++){
            const int t=HitTest(TileRect(i,96,6).x+10, TileRect(i,96,6).y+10, 96, 6);
            ok &= Check(t!=hitNone&&t!=hitSliderTrack&&t!=hitDimNow&&t!=hitFooterExit,L"6-tile: every tile resolves to a tile target");
            for(int j=0;j<i;j++)ok &= Check(seenTargets[j]!=t,L"6-tile: no two tiles share a target");
            seenTargets[i]=t;
        }

        // Hit testing for slider and footer controls with 5 and 6 buttons
        Rect tr5 = SliderTrackRect(96, 5);
        ok &= Check(HitTest(tr5.x+10, tr5.y, 96, 5) == hitSliderTrack, L"5-tile: hit slider track at shifted y");
        ok &= Check(HitTest(DimNowRect(96, 5).x+5, DimNowRect(96, 5).y+5, 96, 5) == hitDimNow, L"5-tile: hit dim now at shifted y");
        ok &= Check(HitTest(FooterExitRect(96, 5).x+5, FooterExitRect(96, 5).y+5, 96, 5) == hitFooterExit, L"5-tile: hit exit at shifted y");

        // AnchorPanel sizing with 5 and 6 buttons
        Rect a5 = AnchorPanel(96, 0, 0, 1920, 1080, 5);
        Rect a6 = AnchorPanel(96, 0, 0, 1920, 1080, 6);
        ok &= Check(a5.h == MulDiv(PanelHeight(5), 96, 96), L"AnchorPanel with 5 buttons matches PanelHeight(5)");
        ok &= Check(a6.h == MulDiv(PanelHeight(6), 96, 96), L"AnchorPanel with 6 buttons matches PanelHeight(6)");

        // TileRect clamps totalButtons > 6 to 6
        ok &= Check(TileRect(4, 96, 7).w == TileRect(4, 96, 6).w, L"TileRect clamps totalButtons > 6 to 6");

        // Slider thumb outer edge hit testing at min and max detents across DPIs
        for(int d=0; d<2; d++){
            int dpi = dpis[d];
            int thumbR = MulDiv(11, dpi, 96);
            int leftThumbEdge = SliderThumbX(dpi, 1) - thumbR + 1;
            int rightThumbEdge = SliderThumbX(dpi, 60) + thumbR - 1;
            int centerY = MulDiv(SliderCenterY(4), dpi, 96);
            ok &= Check(HitTest(leftThumbEdge, centerY, dpi) == hitSliderTrack, L"slider thumb left edge is hittable");
            ok &= Check(HitTest(rightThumbEdge, centerY, dpi) == hitSliderTrack, L"slider thumb right edge is hittable");
        }

        // Multi-DPI pixel sweep for 5 and 6 buttons: every pixel resolves to a known target
        const int reflowSweepDpis[4] = { 96, 120, 144, 192 };
        for(int bc = 5; bc <= 6; bc++){
            for(int d = 0; d < 4; d++){
                int dpi = reflowSweepDpis[d];
                bool allKnown = true;
                int h = MulDiv(PanelHeight(bc), dpi, 96);
                int w = MulDiv(kPanelWidth, dpi, 96);
                for(int y = 0; y < h; y += 4){
                    for(int x = 0; x < w; x += 4){
                        int hit = HitTest(x, y, dpi, bc);
                        if(hit < hitNone || hit > hitFooterExit) allKnown = false;
                    }
                }
                ok &= Check(allKnown, L"reflow panel pixels resolve to known targets across DPIs");
            }
        }
    }
    // The power dashboard card is new in this redesign. Pin its box to the declared stack so
    // it cannot grow past the footer divider or drift outside the panel, at either DPI.
    {
        const int cardDpis[]={96,120,144,168,192,216,240};
        const int cardDpiCount=(int)(sizeof(cardDpis)/sizeof(cardDpis[0]));
        for(int d=0;d<cardDpiCount;d++){int dpi=cardDpis[d];
            Rect card=PowerCardRect(dpi);
            ok &= Check(card.x>=0&&card.x+card.w<=MulDiv(kPanelWidth,dpi,96),L"power card inside panel width");
            ok &= Check(card.y>=MulDiv(TicksDividerY(),dpi,96),L"power card sits below the ticks divider");
            ok &= Check(card.y==MulDiv(TicksDividerY()+kSepH+kCardMarginTop,dpi,96),L"power card top margin matches kCardMarginTop");
            ok &= Check(card.y+card.h<=MulDiv(FooterDividerY(),dpi,96),L"power card fits above the footer divider");
            // Height is the declared stack: padding, three rows, three gaps and the bar.
            ok &= Check(card.h==MulDiv(kCardH,dpi,96),L"power card height matches the declared stack");
            // The bottom margin must not cross into the footer divider (it lands exactly on it).
            ok &= Check(card.y+card.h+MulDiv(kCardMarginBottom,dpi,96)<=MulDiv(FooterDividerY(),dpi,96),L"card bottom margin clears the footer divider");

            // Also verify 5-button and 6-button reflow cards across every DPI
            Rect card5=PowerCardRect(dpi,5);
            ok &= Check(card5.y==MulDiv(TicksDividerY(5)+kSepH+kCardMarginTop,dpi,96),L"5-button reflow card top margin matches kCardMarginTop");
            ok &= Check(card5.y+card5.h+MulDiv(kCardMarginBottom,dpi,96)<=MulDiv(FooterDividerY(5),dpi,96),L"5-button reflow card bottom margin clears footer divider");
            Rect card6=PowerCardRect(dpi,6);
            ok &= Check(card6.y==MulDiv(TicksDividerY(6)+kSepH+kCardMarginTop,dpi,96),L"6-button reflow card top margin matches kCardMarginTop");
            ok &= Check(card6.y+card6.h+MulDiv(kCardMarginBottom,dpi,96)<=MulDiv(FooterDividerY(6),dpi,96),L"6-button reflow card bottom margin clears footer divider");

            Rect tr4=SliderTrackRect(dpi,4);
            ok &= Check(tr4.x>=0 && tr4.x+tr4.w<=MulDiv(kPanelWidth,dpi,96),L"slider track within panel width across DPIs");
            Rect t0=TileRect(0,dpi,4);
            ok &= Check(t0.w>0 && t0.h>0 && t0.x+t0.w<=MulDiv(kPanelWidth,dpi,96),L"tile 0 within panel bounds across DPIs");
        }
    }
    // The footer buttons must not overlap each other or the slider track, and must stay inside
    // the footer band. An overlap makes the draw order decide which control wins - invisible.
    {
        const int footDpis[2]={96,192};
        for(int d=0;d<2;d++){int dpi=footDpis[d];
            Rect dn=DimNowRect(dpi),fe=FooterExitRect(dpi),tr=SliderTrackRect(dpi);
            ok &= Check(dn.x+dn.w<=fe.x||fe.x+fe.w<=dn.x,L"dim-now and exit buttons do not overlap");
            ok &= Check(dn.y>=tr.y+tr.h&&fe.y>=tr.y+tr.h,L"footer buttons clear of the slider track");
            ok &= Check(fe.y+fe.h<=MulDiv(FooterY()+kFooterHeight,dpi,96),L"exit button inside the footer band");
        }
    }
    // The declared panel height must cover the sum of the stack it holds: grid, divider,
    // slider, ticks, card, footer divider and footer.
    ok &= Check(PanelHeight()>=kDividerY+kSepH+kSliderHeight+kTickRowHeight+kSepH+kCardMarginTop+kCardH+kCardMarginBottom+kSepH+kFooterHeight,L"panel height covers the full declared stack");
    ok &= Check(PanelHeight()==kFooterY+kFooterHeight,L"panel height ends at the footer stack end");

    // Auto-hide work areas include the taskbar: reserve its expanded bounds before anchoring.
    {
        const Rect screen={0,0,3240,2160};
        Rect usable=ReserveTaskbarSpace(screen,Rect{0,2064,3240,96},ABE_BOTTOM);
        ok &= Check(usable.h==2064,L"auto-hide bottom taskbar reserves its expanded height");
        Rect panel=AnchorPanel(192,usable.x,usable.y,usable.w,usable.h);
        ok &= Check(panel.y+panel.h<=2064,L"200 percent panel footer clears the auto-hide taskbar");
        Rect again=ReserveTaskbarSpace(usable,Rect{0,2064,3240,96},ABE_BOTTOM);
        ok &= Check(again.h==usable.h,L"already reserved taskbar space is not subtracted twice");
        Rect left=ReserveTaskbarSpace(screen,Rect{0,0,96,2160},ABE_LEFT);
        ok &= Check(left.x==96&&left.w==3144,L"left taskbar excluded from panel work area");
        Rect top=ReserveTaskbarSpace(screen,Rect{0,0,3240,96},ABE_TOP);
        ok &= Check(top.y==96&&top.h==2064,L"top taskbar excluded from panel work area");
        Rect right=ReserveTaskbarSpace(screen,Rect{3144,0,96,2160},ABE_RIGHT);
        ok &= Check(right.w==3144,L"right taskbar excluded from panel work area");
        Rect other=ReserveTaskbarSpace(screen,Rect{-1920,1000,1920,80},ABE_BOTTOM);
        ok &= Check(other.w==screen.w&&other.h==screen.h,L"taskbar on another monitor leaves work area intact");
    }

    // Anchor: the panel must land fully inside the work area for a taskbar on any edge.
    struct Case { int wx,wy,ww,wh; };
    const Case cases[]={
        {0,0,1920,1040},       // taskbar bottom
        {0,48,1920,992},       // taskbar top
        {48,0,1872,1040},      // taskbar left
        {0,0,1880,1040},       // taskbar right
    };
    for(const Case& c : cases){
        Rect r=AnchorPanel(96,c.wx,c.wy,c.ww,c.wh);
        ok &= Check(r.x>=c.wx&&r.y>=c.wy,L"anchor inside work area origin");
        ok &= Check(r.x+r.w<=c.wx+c.ww,L"anchor inside work area width");
        ok &= Check(r.y+r.h<=c.wy+c.wh,L"anchor inside work area height");
        ok &= Check(r.w>0&&r.h>0,L"anchor rect non-degenerate");
    }
    // Work area smaller than the panel. It cannot fit, and demanding that it do would be
    // asserting the impossible; the requirement is that it pins to the work area origin and
    // keeps its size, so the panel stays readable rather than collapsing to a sliver.
    Rect tiny=AnchorPanel(96,0,0,200,150);
    ok &= Check(tiny.x==0&&tiny.y==0,L"undersized work area pins to origin");
    ok &= Check(tiny.w==kPanelWidth&&tiny.h==PanelHeight(),L"undersized work area keeps panel size");

    // DPI scaling is linear and doubles at 200%. A rect that does not scale exactly means a
    // metric was hardcoded instead of passed through S().
    for(int i=0;i<4;i++){
        Rect a=TileRect(i,96),b=TileRect(i,192);
        ok &= Check(b.w==a.w*2&&b.h==a.h*2,L"tile scales linearly with DPI");
    }
    Rect r96=AnchorPanel(96,0,0,1920,1040);
    Rect r192=AnchorPanel(192,0,0,1920,1040);
    ok &= Check(r192.w==r96.w*2&&r192.h==r96.h*2,L"panel scales linearly with DPI");
    ok &= Check(r96.x+r96.w==1920-12&&r96.y+r96.h==1040-12,L"flyout rests at bottom-right with Quick Settings edge gaps");
    ok &= Check(r192.x+r192.w==1920-24&&r192.y+r192.h==1040-24,L"flyout edge gaps scale at 200 percent");
    Rect negative=AnchorPanel(96,-1920,-1080,1920,1040);
    ok &= Check(negative.x+negative.w==-12&&negative.y+negative.h==-52,L"flyout anchor supports negative monitor coordinates");

    ok &= Check(PanelEntranceOffset(0,96)==25,L"flyout entrance begins below its resting position");
    ok &= Check(PanelEntranceOffset(0,192)==50,L"flyout entrance travel scales at 200 percent");
    ok &= Check(PanelEntranceOffset(kPanelEntranceMs,192)==0,L"flyout entrance settles at its final position");
    ok &= Check(PanelEntranceOffset(10000,96)==0,L"delayed flyout frame cannot overshoot or restart");
    bool monotonic=true;
    int previousEntranceOffset=PanelEntranceOffset(0,192);
    for(unsigned int ms=1;ms<=kPanelEntranceMs;ms++){
        int offset=PanelEntranceOffset(ms,192);
        if(offset>previousEntranceOffset||offset<0)monotonic=false;
        previousEntranceOffset=offset;
    }
    ok &= Check(monotonic,L"flyout ease-out approaches its resting position without reversal");
    ok &= Check(PanelExitOffset(0,0,96)==0,L"flyout exit starts at the displayed resting position");
    ok &= Check(PanelExitOffset(kPanelExitMs,0,96)==25,L"flyout exit finishes below its resting position");
    ok &= Check(PanelExitOffset(kPanelExitMs,0,192)==50,L"flyout exit travel scales at 200 percent");
    ok &= Check(PanelExitOffset(0,12,192)==12,L"interrupted entrance closes from its displayed offset without jumping");
    ok &= Check(PanelExitOffset(10000,12,192)==62,L"delayed exit frame finishes its full travel from an interrupted entrance");
    bool exitMonotonic=true;
    int previousExitOffset=PanelExitOffset(0,12,192);
    for(unsigned int ms=1;ms<=kPanelExitMs;ms++){
        int offset=PanelExitOffset(ms,12,192);
        if(offset<previousExitOffset||offset>62)exitMonotonic=false;
        previousExitOffset=offset;
    }
    ok &= Check(exitMonotonic,L"flyout exit moves down continuously without reversal or overshoot");

    // A zero DPI must not collapse geometry; GetDpiForWindow can return 0 before the window
    // is shown, and every rect becoming zero-width would silently draw nothing.
    ok &= Check(TileRect(0,0).w>0,L"zero DPI does not collapse tile");
    ok &= Check(AnchorPanel(0,0,0,1920,1040).w>0,L"zero DPI does not collapse panel");

    return ok;
}

// Session counter assertions. Deliberately here and not in SessionStats.cpp, for the same reason
// FlyoutLayoutSelfTest sits here rather than in Flyout.cpp: these suites run through the
// file-static Check() helper so every individual assertion is counted and named. A bool-returning
// test living inside the module under test could only ever register as ONE check, which would
// satisfy "the count went up" while testing almost nothing (AGENTS.md 5).
static bool SessionStatsChecks() {
    using namespace sessionstats;
    bool ok=true;
    wchar_t buf[32];

    // ---- Reset, and the never-vs-zero sentinel.
    Reset();
    Counters c=Get();
    ok&=Check(c.wakeCount==0&&c.dimCount==0&&c.restoreCount==0,L"stats: reset clears every event counter");
    ok&=Check(c.lastDimTick==0ULL&&c.lastRestoreTick==0ULL,L"stats: reset clears the never-sentinel");
    ok&=Check(c.dimmedMs==0ULL,L"stats: reset clears accumulated dimmed time");
    ok&=Check(!HasDimHistory(c),L"stats: a fresh session has no dim history");

    // ---- Counters increment independently.
    NoteWake();NoteWake();NoteWake();
    c=Get();
    ok&=Check(c.wakeCount==3,L"stats: three wakes counted");
    ok&=Check(c.dimCount==0&&c.restoreCount==0,L"stats: counting wakes does not touch dim/restore");
    NoteDim(1000ULL);
    c=Get();
    ok&=Check(c.dimCount==1,L"stats: one dim counted");
    ok&=Check(c.lastDimTick==1000ULL,L"stats: last dim tick recorded");
    ok&=Check(HasDimHistory(c),L"stats: dim history appears on the first dim");
    ok&=Check(c.wakeCount==3,L"stats: counting a dim does not touch the wake counter");
    NoteRestore(2000ULL);
    c=Get();
    ok&=Check(c.restoreCount==1&&c.lastRestoreTick==2000ULL,L"stats: restore counted with its tick");
    ok&=Check(c.dimCount==1,L"stats: counting a restore does not touch the dim counter");

    // ---- A tick of 0 is the sentinel, not midnight. This is the distinction that keeps an
    // honest blank (em dash) separable from a real zero.
    Reset();
    NoteDim(0ULL);NoteRestore(0ULL);
    c=Get();
    ok&=Check(c.dimCount==0&&c.restoreCount==0,L"stats: a zero tick registers no event");
    ok&=Check(!HasDimHistory(c),L"stats: a zero tick does not invent dim history");

    // ---- Accumulated dimmed time is additive and monotonic.
    Reset();
    AccumulateDimmed(100ULL);AccumulateDimmed(200ULL);
    ok&=Check(Get().dimmedMs==300ULL,L"stats: dimmed time accumulates additively");
    const unsigned long long before=Get().dimmedMs;
    AccumulateDimmed(50ULL);
    ok&=Check(Get().dimmedMs>=before,L"stats: dimmed time never decreases");
    ok&=Check(Get().dimmedMs==350ULL,L"stats: dimmed time totals correctly");

    // ---- Formatting. These are the strings the card prints, so they are contract, not cosmetics.
    FormatHoursSaved(0ULL,buf,32);
    ok&=Check(wcscmp(buf,L"0.0 h")==0,L"stats: zero saved time reads 0.0 h, not 0 h");
    FormatHoursSaved(600000ULL,buf,32);   // 10 min
    ok&=Check(wcscmp(buf,L"0.2 h")==0,L"stats: 10 minutes reads 0.2 h");
    FormatHoursSaved(3600000ULL,buf,32);  // exactly 1 h
    ok&=Check(wcscmp(buf,L"1.0 h")==0,L"stats: one hour reads 1.0 h");
    const int minutes[7]={1,2,5,10,15,30,60};
    bool allMinutes=true;
    for(int i=0;i<7;i++){wchar_t exp[32];swprintf(exp,32,L"%d min",minutes[i]);FormatMinutes(minutes[i],buf,32);if(wcscmp(buf,exp)!=0)allMinutes=false;}
    ok&=Check(allMinutes,L"stats: every slider detent renders its own minute label");
    FormatWhen(180000ULL,false,buf,32);   // 3 min
    ok&=Check(wcscmp(buf,L"3 min")==0,L"stats: an age renders as minutes");
    FormatWhen(9ULL*3600000ULL+14ULL*60000ULL,true,buf,32);
    ok&=Check(wcscmp(buf,L"09:14")==0,L"stats: a wall-clock offset renders HH:MM");

    // ---- Every formatter must survive a hostile buffer. A panel that overflows a stack buffer
    // is a crash in a background tray app with no window to show the error on.
    wchar_t tiny[4];tiny[0]=L'Z';tiny[1]=L'Z';tiny[2]=L'Z';tiny[3]=L'Z';
    const int written=FormatMinutes(60,tiny,4);
    ok&=Check(tiny[3]==L'\0',L"stats: a truncated label is still terminated");
    ok&=Check(written<=3,L"stats: a truncated label never overruns cap");
    ok&=Check(FormatMinutes(5,nullptr,10)==0,L"stats: a null destination is refused, not written");
    ok&=Check(FormatMinutes(5,tiny,0)==0,L"stats: a zero cap is refused, not written");
    wchar_t one[2];one[0]=L'X';one[1]=L'X';
    ok&=Check(FormatMinutes(60,one,1)==0&&one[0]==L'\0',L"stats: a one-slot buffer takes only a terminator");

    Reset();   // leave the module clean for the live app
    return ok;
}

// Scheduler sleep assertions. THE field regression: a decision that declined to dim armed the
// 15 minute backstop and the app never woke again - observed as checks=1 frozen in the debug log
// across a three minute watch, with the threshold long past. Ready-to-dim must wake at human
// cadence so a declined decision can be reconsidered.
// Accent assertions. These pin COLORREF semantics - the documented convention DwmGetColorization-
// Color returns - and the "never resolve to black" rule the fallback exists to satisfy.
//
// The orange regression is the one that matters. MEASURED 2026-10-19: GetSysColor(COLOR_HIGHLIGHT)
// returned a plausible orange that was non-zero, so a "non-zero means valid" guard accepted it and
// the safe fallback never ran - which is exactly how the app painted orange tiles while Windows
// showed the real accent. These assertions are what stop a value merely being non-zero from being
// treated as correct.
static bool NativeOverlayChecks() {
    // Pixel-format contracts, not visual certification: body films must reveal the OS material,
    // opaque accent RGB must survive, and every output channel must be premultiplied by coverage.
    bool ok=true;
    PanelState s={};s.dark=true;s.enabled=true;s.accent=0xD4BB8Bu;s.buttonCount=6;
    const int w=kPanelWidth,h=PanelHeight(6);
    HBITMAP bmp=PaintPanelOverlay(w,h,s,96);
    DIBSECTION dib={};
    const bool rendered=bmp&&GetObject(bmp,sizeof(dib),&dib)&&dib.dsBm.bmBits;
    ok&=Check(rendered,L"native overlay: creates BGRA control surface");
    if(rendered){
        BYTE* pixels=(BYTE*)dib.dsBm.bmBits;
        const Rect tile=TileRect(0,96,6);
        BYTE* accent=pixels+((tile.y+10)*w+tile.x+10)*4;
        ok&=Check(accent[0]==0x8B&&accent[1]==0xBB&&accent[2]==0xD4&&accent[3]==255,
                  L"native overlay: opaque accent retains BGRA channels");
        // Gap below the tile labels, away from dividers and all controls.
        BYTE* body=pixels+((DividerY(6)-5)*w+w/2)*4;
        ok&=Check(body[3]>0&&body[3]<16&&body[0]==body[3]&&body[1]==body[3]&&body[2]==body[3],
                  L"native overlay: body has a thin white film over the material");
        BYTE* footer=pixels+((FooterY(6)+5)*w+w/2)*4;
        ok&=Check(footer[3]==0&&footer[0]==0&&footer[1]==0&&footer[2]==0,
                  L"native overlay: footer keeps the bare material");
        BYTE* seam=pixels+(FooterDividerY(6)*w+w/2)*4;
        ok&=Check(seam[3]>0&&seam[3]<32&&seam[0]==0&&seam[1]==0&&seam[2]==0,
                  L"native overlay: footer divider is a translucent dark seam");
        const Rect off=TileRect(1,96,6);
        BYTE* film=pixels+((off.y+10)*w+off.x+10)*4;
        ok&=Check(film[3]>0&&film[3]<32,L"native overlay: OFF tile remains a translucent film");
        bool premultiplied=true,hasCoverage=false;
        for(int i=0;i<w*h;i++){
            BYTE* p=pixels+i*4;
            if(p[0]>p[3]||p[1]>p[3]||p[2]>p[3])premultiplied=false;
            if(p[3]>0&&p[3]<255)hasCoverage=true;
        }
        ok&=Check(premultiplied,L"native overlay: all RGB channels are premultiplied");
        ok&=Check(hasCoverage,L"native overlay: fractional alpha is preserved");
    }
    if(bmp)DeleteObject(bmp);
    return ok;
}
static bool AccentChecks() {
    bool ok=true;
    // COLORREF is 0x00BBGGRR: low byte red, then green, then blue. The panel wants 0x00RRGGBB.
    ok&=Check(AccentFromColorRef(0x00D47800u)==0x0078D4u,L"accent: COLORREF #0078D4 converts to RGB");
    ok&=Check(AccentFromColorRef(0x00FFC24Cu)==0x4CC2FFu,L"accent: COLORREF #4CC2FF converts to RGB");
    // Orange, the accent this machine actually showed: red must stay red, not become blue.
    ok&=Check(AccentFromColorRef(0x000088F7u)==0xF78800u,L"accent: an orange accent is not swapped to blue");
    ok&=Check((AccentFromColorRef(0x000088F7u)>>16&0xFF)==0xF7u,L"accent: red lands in the high byte");
    ok&=Check((AccentFromColorRef(0x000088F7u)&0xFF)==0x00u,L"accent: blue lands in the low byte");
    // The rule the fallback exists for: nothing may reach the painter as black.
    ok&=Check(AccentFromColorRef(0)==0,L"accent: zero converts to zero so the caller can reject it");
    ok&=Check(AccentFromColorRef(0x00000000u)==0,L"accent: an all-zero accent is rejected, not painted");
    // The AccentPalette, byte for byte, MEASURED 2026-10-24 on this machine: eight 4-byte entries
    // light to dark. Entry 1 is the resting tile fill the user measured off Windows' own Quick
    // Settings (#D4BB8B); entry 0 is the lightest shade (#ECDCAA). The previous fixture pinned
    // entry 0 as the accent because a pointing tool was resting on a hovered tile when the value
    // was read - a green assertion proving the wrong swatch. Built from observed bytes rather than
    // a DWORD literal copied out of a neighbouring value.
    const BYTE measured[16]={0xEC,0xDC,0xAA,0x00, 0xD4,0xBB,0x8B,0x00,
                             0xB2,0x82,0x49,0x00, 0x98,0x6B,0x3E,0x00};
    ok&=Check(AccentFromPalette(measured,16,1)==0xD4BB8Bu,L"accent: AccentPalette[1] is the tile fill #D4BB8B");
    ok&=Check(AccentFromPalette(measured,16,0)==0xECDCAAu,L"accent: AccentPalette[0] is the lighter shade #ECDCAA");
    ok&=Check(AccentFromPalette(measured,16,1)!=AccentFromPalette(measured,16,0),L"accent: the tile fill is not the lightest shade");
    ok&=Check(AccentFromPalette(measured,16,2)==0xB28249u,L"accent: entry 2 reads as its own shade");
    // Short and absent palettes must be refused rather than read out of bounds. Entry 1 needs
    // seven bytes, so a six-byte palette must fail instead of reading a partial entry.
    ok&=Check(AccentFromPalette(measured,6,1)==0,L"accent: a palette too short for entry 1 is refused");
    // Indices outside a complete palette must be refused, not clamped.
    ok&=Check(AccentFromPalette(measured,16,8)==0,L"accent: an index past the palette is refused");
    ok&=Check(AccentFromPalette(measured,16,-1)==0,L"accent: a negative index is refused");
    ok&=Check(AccentFromPalette(nullptr,16,1)==0,L"accent: a null palette is refused");
    ok&=Check(AccentFromPalette(measured,2,0)==0,L"accent: a truncated palette is refused");
    ok&=Check(AccentFromPalette(measured,0,0)==0,L"accent: an empty palette is refused");
    // COLORREF conversion, still used by the tray-icon tint path.
    ok&=Check(AccentFromColorRef(0xE3806456u)==0x566480u,L"accent: COLORREF #566480 converts to RGB");
    ok&=Check(AccentFromColorRef(RGB(0x8C,0x00,0xE3))==0x8C00E3u,L"accent: a magenta COLORREF converts to #8C00E3");
    // And the fallbacks themselves must be usable, non-black, and distinct.
    ok&=Check((theme::accentDark().r|theme::accentDark().g|theme::accentDark().b)!=0,L"accent: the dark fallback is not black");
    ok&=Check((theme::accentLight().r|theme::accentLight().g|theme::accentLight().b)!=0,L"accent: the light fallback is not black");
    // Painted surface check: paint the panel into a 32-bpp DIB and sample a pixel inside tile 0.
    // Proves that PaintPanel composites the exact RGB accent into memory without swapping Red and Blue.
    {
        HDC screen=GetDC(nullptr);
        HDC mem=screen?CreateCompatibleDC(screen):nullptr;
        BITMAPINFO bi={};
        bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth=kPanelWidth;
        bi.bmiHeader.biHeight=-PanelHeight();
        bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;
        void* bits=nullptr;
        HBITMAP bmp=mem?CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        if(bmp&&mem){
            HGDIOBJ old=SelectObject(mem,bmp);
            PanelState s={};
            s.enabled=true;
            s.accent=0xD4BB8Bu;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            Rect t=TileRect(0,96);
            COLORREF px=GetPixel(mem,t.x+10,t.y+10);
            unsigned int paintedRgb=((unsigned int)GetRValue(px)<<16)|((unsigned int)GetGValue(px)<<8)|GetBValue(px);
            ok&=Check(paintedRgb==0xD4BB8Bu,L"accent: painted tile pixel is #D4BB8B, not byte-reversed");
            ok&=Check(paintedRgb!=0x8BBBD4u,L"accent: painted tile pixel is not byte-reversed #8BBBD4");
            // Hover paints the highlight swatch from AccentPalette[0], so the pixel must be exactly
            // #ECDCAA - not the (+24,+33,+31) lift that entry 0 could not be reproduced with.
            s.accentHover=0xECDCAAu;
            s.hover=hitTileDimming;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            px=GetPixel(mem,t.x+10,t.y+10);
            const unsigned int hoverRgb=((unsigned int)GetRValue(px)<<16)|((unsigned int)GetGValue(px)<<8)|GetBValue(px);
            ok&=Check(hoverRgb==0xECDCAAu,L"accent: a hovered tile paints the highlight #ECDCAA");
            ok&=Check(hoverRgb!=paintedRgb,L"accent: hover is visibly different from rest");
            // Held steps once further so a press is distinguishable from hover. That step is a UI
            // choice, not a measured Windows value, so it is only pinned to differ.
            s.pressed=hitTileDimming;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            px=GetPixel(mem,t.x+10,t.y+10);
            const unsigned int heldRgb=((unsigned int)GetRValue(px)<<16)|((unsigned int)GetGValue(px)<<8)|GetBValue(px);
            ok&=Check(heldRgb!=hoverRgb,L"accent: the press state differs from hover");
            SelectObject(mem,old);
            DeleteObject(bmp);
        }
        if(mem)DeleteDC(mem);
        if(screen)ReleaseDC(nullptr,screen);
    }
    // The hover state must actually reach the painter. `panelHover` is tracked by WM_MOUSEMOVE and
    // PaintPanel honours state.hover, but CurrentPanelState copied neither - so every frame was
    // painted with hitNone and no tile could ever highlight. Asserting the copy is the only check
    // that sees a wiring omission: the painted-surface tests above supply hover by hand and stay
    // green either way.
    {
        const int savedHover=panelHover, savedPressed=panelPressed;
        panelHover=hitTileMedia;
        panelPressed=hitTileDimming;
        PanelState live={};
        CurrentPanelState(live);
        ok&=Check(live.hover==hitTileMedia,L"panel: the live state carries the hovered control");
        ok&=Check(live.pressed==hitTileDimming,L"panel: the live state carries the pressed control");
        panelHover=savedHover;
        panelPressed=savedPressed;
    }
    // Resource leak check: paint repeatedly through PaintPanel and confirm GDI handle count stays flat
    {
        HDC screen=GetDC(nullptr);
        HDC mem=screen?CreateCompatibleDC(screen):nullptr;
        BITMAPINFO bi={};
        bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth=kPanelWidth;
        bi.bmiHeader.biHeight=-PanelHeight();
        bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;
        void* bits=nullptr;
        HBITMAP bmp=mem?CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        if(bmp&&mem){
            HGDIOBJ old=SelectObject(mem,bmp);
            PanelState s={};
            s.enabled=true;
            s.accent=0xEADFCDu;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),0);
            for(int iter=0;iter<10;iter++){
                s.minutes=(iter%60)+1;
                PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            }
            DWORD gdiAfter=GetGuiResources(GetCurrentProcess(),0);
            ok&=Check(gdiAfter==gdiBefore,L"PaintPanel: zero GDI handle leak over repeated repaints");
            SelectObject(mem,old);
            DeleteObject(bmp);
        }
        if(mem)DeleteDC(mem);
        if(screen)ReleaseDC(nullptr,screen);
    }
    // Dynamic contrast luminance test: bright accent gives dark glyph, dark accent gives light glyph
    {
        // #EADFCD luminance: (234*299 + 223*587 + 205*114)/1000 = 224 > 140
        int lumBright=(0xEA*299 + 0xDF*587 + 0xCD*114)/1000;
        ok&=Check(lumBright>140,L"accent contrast: #EADFCD is high luminance (>140)");
        // #0078D4 luminance: (0*299 + 120*587 + 212*114)/1000 = 94 <= 140
        int lumDark=(0*299 + 120*587 + 212*114)/1000;
        ok&=Check(lumDark<=140,L"accent contrast: #0078D4 is low luminance (<=140)");
    }
    // Footer repaint resource checks across light, dark and disabled panel states.
    {
        HDC screen=GetDC(nullptr);
        HDC mem=screen?CreateCompatibleDC(screen):nullptr;
        BITMAPINFO bi={};
        bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth=kPanelWidth;
        bi.bmiHeader.biHeight=-PanelHeight();
        bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;
        void* bits=nullptr;
        HBITMAP bmp=mem?CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        if(bmp&&mem){
            HGDIOBJ old=SelectObject(mem,bmp);
            PanelState s={};
            DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),0);
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            ok&=Check(GetGuiResources(GetCurrentProcess(),0)==gdiBefore,L"PaintPanel: light footer repaint releases GDI handles");
            s.dark=true;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            ok&=Check(GetGuiResources(GetCurrentProcess(),0)==gdiBefore,L"PaintPanel: dark footer repaint releases GDI handles");
            s.enabled=true;
            PaintPanel(mem,kPanelWidth,PanelHeight(),s,96);
            ok&=Check(GetGuiResources(GetCurrentProcess(),0)==gdiBefore,L"PaintPanel: enabled footer repaint releases GDI handles");
            SelectObject(mem,old);
            DeleteObject(bmp);
        }
        if(mem)DeleteDC(mem);
        if(screen)ReleaseDC(nullptr,screen);
    }
    // SessionStats FormatWhen: elapsed minutes and timestamp clock formatting
    {
        wchar_t buf[32];
        sessionstats::FormatWhen(0ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"0 min")==0,L"FormatWhen: 0 ms formats as '0 min'");
        sessionstats::FormatWhen(120000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"2 min")==0,L"FormatWhen: 120000 ms formats as '2 min'");
        sessionstats::FormatWhen(3600000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"60 min")==0,L"FormatWhen: 1 hour formats as '60 min'");
        sessionstats::FormatWhen(180000ULL,true,buf,32);
        ok&=Check(wcscmp(buf,L"00:03")==0,L"FormatWhen: past 180000 ms formats as '00:03'");
    }
    // "Last dim" is a plain minute count ("N min"). Minutes never roll
    // up into hours or days: the user asked for minutes, so a dim from yesterday reads as a
    // large minute count rather than a clock time.
    {
        wchar_t buf[32];
        sessionstats::FormatWhen(59999ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"0 min")==0,L"LastDim: 59 s is 0 min");
        sessionstats::FormatWhen(60000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"1 min")==0,L"LastDim: one minute reads '1 min'");
        sessionstats::FormatWhen(59ULL*60000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"59 min")==0,L"LastDim: 59 minutes stays minutes");
        sessionstats::FormatWhen(24ULL*3600000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"1440 min")==0,L"LastDim: a day is still minutes, not hours");
        sessionstats::FormatWhen(1581ULL*60000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"1581 min")==0,L"LastDim: a day-old dim is a minute count");
        sessionstats::FormatWhen(400ULL*86400000ULL,false,buf,32);
        ok&=Check(wcscmp(buf,L"576000 min")==0,L"LastDim: very old dims stay minutes");
        ok&=Check(sessionstats::FormatWhen(0ULL,false,nullptr,32)==0,L"LastDim: a null destination is refused");
        wchar_t one[2];one[0]=L'X';one[1]=L'X';
        ok&=Check(sessionstats::FormatWhen(0ULL,false,one,1)==0&&one[0]==L'\0',L"LastDim: a one-slot buffer takes only a terminator");
        ok&=Check(sessionstats::FormatWhen(0ULL,false,buf,0)==0,L"LastDim: a zero cap is refused");
    }
    // Top button glyph rendering: verify each tile contains actual drawn glyph vector pixels
    {
        HDC screen=GetDC(nullptr);
        HDC mem=screen?CreateCompatibleDC(screen):nullptr;
        BITMAPINFO bi={};
        bi.bmiHeader.biSize=sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth=kPanelWidth;
        bi.bmiHeader.biHeight=-PanelHeight(6);
        bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;
        void* bits=nullptr;
        HBITMAP bmp=mem?CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        if(bmp&&mem){
            HGDIOBJ old=SelectObject(mem,bmp);
            PanelState s={};
            s.enabled=true; s.mediaGuard=true; s.soundGuard=true; s.fullscreenGuard=true; s.offMode=false; s.autostart=true;
            s.accent=0xEADFCDu; // bright accent -> onAccent is black (RGB 0,0,0)
            s.buttonCount=4;
            PaintPanel(mem,kPanelWidth,PanelHeight(4),s,96);
            for(int i=0;i<4;i++){
                Rect t=TileRect(i,96,4);
                int glyphPixels=0, edgePixels=0;
                const COLORREF fill=GetPixel(mem,t.x+t.w/2,t.y+3);
                for(int dy=-7;dy<=7;dy++){
                    for(int dx=-7;dx<=7;dx++){
                        COLORREF px=GetPixel(mem,t.x+t.w/2+dx,t.y+t.h/2+dy);
                        const int r=GetRValue(px),g=GetGValue(px),b=GetBValue(px);
                        // The glyph is dark on this bright accent. Ink is any pixel that
                        // differs from the tile fill; a pixel that is not a full dark core
                        // is edge coverage, which is the property the old integer-line
                        // painter lacked. Media's 1.0 px bars are deliberately all edge
                        // coverage at 96 DPI - two partial columns, no solid core - so
                        // requiring a solid core would reject correct antialiasing.
                        if(px!=fill){
                            glyphPixels++;
                            if(!(r<48&&g<48&&b<48)) edgePixels++;
                        }
                    }
                }
                ok&=Check(glyphPixels>0,i==0?L"glyph: tile 0 (Dimming/IC.sun) rendered strokes":
                                      i==1?L"glyph: tile 1 (Media/IC.media) rendered strokes":
                                      i==2?L"glyph: tile 2 (Fullscreen/IC.full) rendered strokes":
                                           L"glyph: tile 3 (Power/IC.power) rendered strokes");
                ok&=Check(edgePixels>0,i==0?L"glyph: tile 0 has antialiased edge coverage":
                                      i==1?L"glyph: tile 1 has antialiased edge coverage":
                                      i==2?L"glyph: tile 2 has antialiased edge coverage":
                                           L"glyph: tile 3 has antialiased edge coverage");
            }
            // Verify 6-button reflow for tile 5 (Folder/IC.folder)
            s.dark=true;
            s.buttonCount=6;
            PaintPanel(mem,kPanelWidth,PanelHeight(6),s,96);
            Rect tFolder=TileRect(5,96,6);
            int folderPixels=0, folderEdges=0;
            const COLORREF folderFill=GetPixel(mem,tFolder.x+tFolder.w/2,tFolder.y+3);
            for(int dy=-7;dy<=7;dy++){
                for(int dx=-7;dx<=7;dx++){
                    COLORREF px=GetPixel(mem,tFolder.x+tFolder.w/2+dx,tFolder.y+tFolder.h/2+dy);
                    const int r=GetRValue(px),g=GetGValue(px),b=GetBValue(px);
                    // On the dark theme's inactive tile the glyph is white. The classic
                    // failure was a binary white core with no intermediate coverage.
                    if(px!=folderFill){
                        folderPixels++;
                        if(!(r>207&&g>207&&b>207)) folderEdges++;
                    }
                }
            }
            ok&=Check(folderPixels>0,L"glyph: tile 5 (Folder/IC.folder) rendered strokes");
            ok&=Check(folderEdges>0,L"glyph: tile 5 (Folder/IC.folder) has antialiased edge coverage");
            // The bottom ribbon is its own opaque plate, not a translucent tint over the panel
            // base. Sampled from a painted surface because that is the only way to prove the
            // blend did not sneak back in: a tint over #2C2C2C can never land on #222222.
            {
                s.dark=true;
                PaintPanel(mem,kPanelWidth,PanelHeight(6),s,96);
                COLORREF px=GetPixel(mem,4,FooterY(6)+4);
                const unsigned int dockRgb=((unsigned int)GetRValue(px)<<16)|((unsigned int)GetGValue(px)<<8)|GetBValue(px);
                ok&=Check(dockRgb==0x222222u,L"dock: the dark ribbon paints exactly #222222 (Windows' reported value)");
                s.dark=false;
                PaintPanel(mem,kPanelWidth,PanelHeight(6),s,96);
                px=GetPixel(mem,4,FooterY(6)+4);
                const unsigned int lightRgb=((unsigned int)GetRValue(px)<<16)|((unsigned int)GetGValue(px)<<8)|GetBValue(px);
                ok&=Check(lightRgb==0xF9F9F9u,L"dock: the light ribbon paints #F9F9F9");
                s.dark=true;
            }
            SelectObject(mem,old);
            DeleteObject(bmp);
        }
        if(mem)DeleteDC(mem);
        if(screen)ReleaseDC(nullptr,screen);
    }
    return ok;
}

// Persistence assertions. The counters survive a restart now, which means one of them is read
// from a file and has to defend itself: a backwards clock step must not turn into an absurd age.
static bool PersistedStatsChecks() {
    bool ok=true;
    ok&=Check(WallDeltaMs(1000,400)==600,L"persist: wall delta is the plain difference");
    ok&=Check(WallDeltaMs(400,400)==0,L"persist: a zero-length wall delta is zero");
    // The regression: unsigned subtraction of a backwards clock step underflows to near 2^64.
    ok&=Check(WallDeltaMs(400,1000)==0,L"persist: a backwards clock step saturates, never underflows");
    ok&=Check(WallDeltaMs(0,~0ULL)==0,L"persist: the worst backwards step is still zero");
    ok&=Check(WallDeltaMs(~0ULL,~0ULL-1)==1,L"persist: a forward step at the top of the range works");
    // And it must never produce a value large enough to render as a plausible-but-fake age.
    ok&=Check(WallDeltaMs(400,1000)<1000,L"persist: a backwards step stays in a believable range");
    return ok;
}

static bool ScheduleChecks() {
    bool ok=true;
    const DWORD kThr=60000;
    // dimmed=false throughout: these describe when a dim decision must be reconsidered.
    ok&=Check(ScheduleWaitMs(kThr,kThr,false,0,false)==kCheckReadyMs,L"sched: ready-to-dim waits the ready cadence, not the backstop");
    ok&=Check(ScheduleWaitMs(60062,kThr,false,0,false)==kCheckReadyMs,L"sched: idle just past the threshold is ready");
    ok&=Check(ScheduleWaitMs(3600000,kThr,false,0,false)==kCheckReadyMs,L"sched: long-idle is ready, not the 15 min backstop");
    ok&=Check(ScheduleWaitMs(0,kThr,false,0,false)==kThr,L"sched: fresh input waits the full threshold");
    ok&=Check(ScheduleWaitMs(30000,kThr,false,0,false)==30000,L"sched: half-countdown waits the remainder");
    // Grace outstanding must still shorten the sleep, or a guard pause could not recover promptly.
    ok&=Check(ScheduleWaitMs(60062,kThr,true,1000,false)==59000,L"sched: outstanding grace waits out the grace horizon");
    ok&=Check(ScheduleWaitMs(0,kThr,true,1000,false)==59000,L"sched: grace horizon wins when longer than idle");
    ok&=Check(ScheduleWaitMs(30000,kThr,true,1000,false)==30000,L"sched: idle horizon wins when shorter than grace");
    // Clamps: never busy-spin, never exceed the backstop.
    ok&=Check(ScheduleWaitMs(59999,kThr,false,0,false)>=kCheckMinMs,L"sched: a one-millisecond remainder is clamped");
    ok&=Check(ScheduleWaitMs(0,kThr,true,0,false)>=kCheckMinMs,L"sched: expired grace is clamped, not zero");
    // A grace period that has already run its course does NOT clear itself: the guard still vetoes,
    // because "no recent input" is precisely what a stuck grace looks like, and silently expiring
    // it would dim through a pause the app never actually took. An earlier version of this test
    // asserted the opposite - that a long-expired grace "becomes ready" - and it passed against the
    // first cut of ScheduleWaitMs only because that cut conflated the two. Asserted here so the
    // distinction is pinned rather than re-litigated.
    ok&=Check(ScheduleWaitMs(3600000,kThr,true,3600000,false)==kCheckMinMs,L"sched: a long-expired grace stays vetoed, not silently cleared");
    // The assertion the field bug violated, stated directly.
    ok&=Check(ScheduleWaitMs(kThr,kThr,false,0,false)<kCheckMaxMs,L"sched: ready-to-dim never sleeps for the backstop");
    // While dimmed, the wake must be as rare as possible. The regression this pins: the first
    // version returned kCheckReadyMs here as well and measured 250 ms gaps (median 265 ms), i.e.
    // the never-dims fix had turned the app into a 4 Hz poller - the exact cost it exists to avoid.
    ok&=Check(ScheduleWaitMs(3600000,kThr,false,0,true)==kCheckDimmedMs,L"sched: while dimmed the wake is the slow one");
    ok&=Check(ScheduleWaitMs(0,kThr,false,0,true)==kCheckDimmedMs,L"sched: dimmed ignores the idle countdown");
    manualDimIgnoreUntil=2000ULL;
    ok&=Check(1000ULL<manualDimIgnoreUntil,L"manual dim: input during 2s grace is ignored");
    ok&=Check(2001ULL>=manualDimIgnoreUntil,L"manual dim: input after 2s grace triggers restore");
    manualDimIgnoreUntil=0ULL;
    manualDim=true;
    ok&=Check(manualDim,L"manual dim: active state holds across session");
    ok&=Check(!(!manualDim&&5000<kThr),L"manual dim: idle < threshold does not trigger scheduler restore");
    manualDim=false;
    ok&=Check(!manualDim,L"manual dim: cleared after restore");
    ok&=Check(ScheduleWaitMs(3600000,kThr,true,1000,true)==kCheckDimmedMs,L"sched: dimmed outranks an outstanding grace");
    ok&=Check(kCheckDimmedMs>kCheckReadyMs,L"sched: the dimmed cadence is slower than the ready cadence");
    ok&=Check(kCheckDimmedMs>kCheckMinMs*10,L"sched: the dimmed cadence is nowhere near a busy-spin");
    // And the constants themselves, so the comparisons above mean what they claim.
    ok&=Check(kCheckMaxMs==15*60*1000,L"sched: the backstop is 15 minutes");
    ok&=Check(kCheckReadyMs<kCheckMaxMs,L"sched: the ready cadence is shorter than the backstop");
    return ok;
}

// Full-screen guard assertions. The regression case is the point: a maximised browser must not
// read as full-screen, because on this machine that mistake is what stopped the app dimming at
// all. The style values and rects are the ones measured on 2026-10-17, not invented.
static bool ScreenGuardChecks() {
    using namespace guard;
    bool ok=true;
    FgInfo f;

    // --- The measured false positive. This is the assertion that must never be deleted.
    f=FgInfo{};
    f.resolved=true;
    f.style=0x15C70000;    // WS_VISIBLE|WS_MAXIMIZE|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX
    f.rect={-7,-7,1626,1086};
    f.monitor={0,0,1620,1080};
    ok&=Check(!LooksFullScreen(f),L"guard: a maximised browser is not full-screen");
    // The cover test alone WOULD have passed it - asserted so the test cannot quietly become
    // vacuous if the style checks are ever reordered or removed.
    ok&=Check(f.rect.left<=f.monitor.left&&f.rect.top<=f.monitor.top&&
              f.rect.right>=f.monitor.right&&f.rect.bottom>=f.monitor.bottom,
              L"guard: the maximised window does cover the monitor (precondition)");
    ok&=Check((f.style&kStyleMaximize)!=0,L"guard: the maximised fixture really carries WS_MAXIMIZE");

    // --- Real full-screen must still be detected.
    f.style=kStylePopup|kStyleVisible;   // styles stripped, as a full-screen title leaves them
    f.rect={0,0,1620,1080};
    ok&=Check(LooksFullScreen(f),L"guard: a full-screen popup covering the monitor is full-screen");
    // Exclusive full-screen commonly overshoots by a pixel of invisible border; equality would
    // have missed it.
    f.rect={-1,-1,1621,1081};
    ok&=Check(LooksFullScreen(f),L"guard: a 1px-overshooting full-screen window is still full-screen");

    // --- Each individual exclusion.
    f=FgInfo{};
    ok&=Check(!LooksFullScreen(f),L"guard: an unresolved foreground window is not full-screen");
    f.resolved=true;f.shellSurface=true;f.style=kStylePopup|kStyleVisible;f.rect={0,0,1620,1080};
    ok&=Check(!LooksFullScreen(f),L"guard: a shell surface is not full-screen");
    f.shellSurface=false;f.cloaked=true;
    ok&=Check(!LooksFullScreen(f),L"guard: a cloaked window is not full-screen");
    f.cloaked=false;f.style=kStylePopup|kStyleVisible|kStyleMaximize;
    ok&=Check(!LooksFullScreen(f),L"guard: WS_MAXIMIZE alone disqualifies a covering window");
    f.style=kStylePopup|kStyleVisible|kStyleCaption;
    ok&=Check(!LooksFullScreen(f),L"guard: a surviving caption disqualifies a covering window");
    f.style=kStylePopup|kStyleVisible|kStyleThickFrame;
    ok&=Check(!LooksFullScreen(f),L"guard: a surviving sizing border disqualifies a covering window");
    // A window that does not cover the monitor is just a window, whatever its style. Note the
    // monitor must be set here: the previous case left f.monitor at the 1620x1080 value and this
    // rect genuinely fails to cover it, so the assertion is only meaningful with both present.
    f.style=kStylePopup|kStyleVisible;f.rect={0,0,800,600};f.monitor={0,0,1620,1080};
    ok&=Check(!LooksFullScreen(f),L"guard: a small borderless window is not full-screen");

    // --- A maximised window whose furniture survived is rejected twice over; both paths agree.
    f.style=kStyleMaximize|kStylePopup|kStyleVisible;f.rect={0,0,1620,1080};f.monitor={0,0,1620,1080};
    ok&=Check(!LooksFullScreen(f),L"guard: maximised borderless is rejected");

    // --- And the cover test still has teeth when every style exclusion has been cleared.
    f.style=0;f.rect={0,0,1620,1080};f.monitor={0,0,1620,1080};
    ok&=Check(LooksFullScreen(f),L"guard: a bare covering window with no exclusions reads full-screen");

    return ok;
}

static int SelfTest() {
    bool ok=true;

    // Tray gesture classification. Under NOTIFYICON_VERSION_4, LOWORD(lParam) is the
    // notification code. Only real user gestures may open the panel; everything else must be
    // ignored, or the panel opens by itself ~10 s after launch when the balloon appears.
    // The codes below are the ones this build's own shellapi.h defines.
    ok &= Check(ClassifyTrayMessage(WM_LBUTTONUP)==trayToggle,L"left click toggles");
    ok &= Check(ClassifyTrayMessage(NIN_SELECT)==trayToggle,L"activate toggles");
    ok &= Check(ClassifyTrayMessage(NIN_KEYSELECT)==trayToggle,L"keyboard select toggles");
    ok &= Check(ClassifyTrayMessage(WM_CONTEXTMENU)==trayToggle,L"context menu toggles");
    // Balloon and popup lifecycle events are NOT gestures. Regression guard: 0x402 and 0x404
    // were measured arriving ~10 s after launch and must never open the panel.
    ok &= Check(ClassifyTrayMessage(NIN_BALLOONSHOW)==trayIgnore,L"balloon show ignored");
    ok &= Check(ClassifyTrayMessage(NIN_BALLOONTIMEOUT)==trayIgnore,L"balloon timeout ignored");
    ok &= Check(ClassifyTrayMessage(NIN_POPUPOPEN)==trayIgnore,L"popup open ignored");
    ok &= Check(ClassifyTrayMessage(NIN_POPUPCLOSE)==trayIgnore,L"popup close ignored");
    ok &= Check(ClassifyTrayMessage(WM_MOUSEMOVE)==trayIgnore,L"hover ignored");
    ok &= Check(ClassifyTrayMessage(0)==trayIgnore,L"code 0 ignored");
    ok &= Check(ClassifyTrayMessage(0xFFFF)==trayIgnore,L"unknown code ignored");

    // One physical left click arrives as WM_LBUTTONUP followed by NIN_SELECT ~12 ms later.
    // Both are gestures on their own, but together they are ONE click, so the second must
    // not toggle again. Measured 2026-10-15 from the live app (514 then 1024, same cursor
    // anchor, 12-47 ms apart); this is the flicker: open, then closed inside a frame.
    ok &= Check(ResolveTrayGesture(WM_LBUTTONUP,0,100000,0)==trayToggle,L"left click 1 opens");
    ok &= Check(ResolveTrayGesture(NIN_SELECT,WM_LBUTTONUP,12,1)==trayIgnore,L"its activation is ignored");
    // A right click is a single code and must always toggle.
    ok &= Check(ResolveTrayGesture(WM_CONTEXTMENU,0,100000,0)==trayToggle,L"context menu toggles");
    // A genuinely separate later click must toggle again.
    ok &= Check(ResolveTrayGesture(WM_LBUTTONUP,NIN_SELECT,5000,1)==trayToggle,L"a later click toggles");
    // Reverse order must also collapse to one gesture.
    ok &= Check(ResolveTrayGesture(WM_LBUTTONUP,NIN_SELECT,8,1)==trayIgnore,L"reverse pair is one gesture");
    // Balloon and popup lifecycle events are never gestures.
    ok &= Check(ResolveTrayGesture(NIN_BALLOONSHOW,WM_LBUTTONUP,12,1)==trayIgnore,L"balloon show after a click");
    ok &= Check(ResolveTrayGesture(NIN_BALLOONTIMEOUT,WM_LBUTTONUP,12,1)==trayIgnore,L"balloon timeout after a click");
    ok &= Check(ResolveTrayGesture(NIN_POPUPOPEN,WM_LBUTTONUP,12,1)==trayIgnore,L"popup open after a click");
    // Keyboard activation must keep working.
    ok &= Check(ResolveTrayGesture(NIN_KEYSELECT,0,100000,0)==trayToggle,L"keyboard select toggles");
    ok &= Check(TrayPressClosesPanel(WM_LBUTTONUP,true),L"tray release closes after a press on the open panel's icon");
    ok &= Check(TrayPressClosesPanel(WM_CONTEXTMENU,true),L"right tray release closes after a press on the open panel's icon");
    ok &= Check(!TrayPressClosesPanel(WM_LBUTTONUP,false),L"tray press begun closed may open normally");
    ok &= Check(!TrayPressClosesPanel(NIN_KEYSELECT,true),L"keyboard activation is not mistaken for an old mouse press");
    ok &= Check(ResolveTrayGesture(NIN_SELECT,WM_LBUTTONUP,12,0)==trayIgnore,L"activation paired with a close remains ignored after the final hide");

    // The click-away hook's own rect test. PanelMouseHook cannot be unit-tested without a
    // real message pump, so the predicate it uses is extracted and pinned here instead. The
    // right/bottom edge is exclusive: a click exactly on the window border belongs to
    // whatever is underneath, which is the documented "the click is not swallowed" behaviour.
    ok &= Check(RectContainsPoint(RECT{10,10,20,20},POINT{15,15}),L"point inside rect");
    ok &= Check(!RectContainsPoint(RECT{10,10,20,20},POINT{9,15}),L"point left of rect");
    ok &= Check(!RectContainsPoint(RECT{10,10,20,20},POINT{20,15}),L"right edge is exclusive");
    ok &= Check(!RectContainsPoint(RECT{10,10,20,20},POINT{15,20}),L"bottom edge is exclusive");

    ok &= !ShouldDim(true,false,119999,200000,0,120000,false,false);
    ok &= ShouldDim(true,false,120000,200000,0,120000,false,false);
    ok &= !ShouldDim(true,false,200000,300000,0,300000,false,false); // custom threshold not yet reached
    ok &= ShouldDim(true,false,300000,400000,0,300000,false,false);  // custom threshold reached
    ok &= !ShouldDim(false,false,300000,300000,0,120000,false,false);
    ok &= !ShouldDim(true,true,300000,300000,0,120000,false,false);
    ok &= !ShouldDim(true,false,300000,300000,0,120000,true,false);
    ok &= !ShouldDim(true,false,300000,300000,0,120000,false,true);
    ok &= !ShouldDim(true,false,300000,300000,400000,120000,false,false);
    ok &= ClampIdleMinutes(0)==1 && ClampIdleMinutes(61)==60 && ClampIdleMinutes(5)==5;
    DWORD wrapped=(DWORD)(50u-0xfffffff0u);ok &= wrapped==66;
    ok &= ClampIconDesign(0)==1 && ClampIconDesign(kIconDesignCount+1)==kIconDesignCount && ClampIconDesign(kIconDesignCount)==kIconDesignCount && ClampIconDesign(2)==2;
    // settings.ini: the reader must survive the exact file this machine has, and the writer
    // must be unable to recreate it. That file carried a UTF-8 BOM and two [Settings]
    // sections, because the profile API cannot see a section behind a BOM and appended a
    // clean one beside the invisible one.
    {
        Settings defaults=DefaultSettings();
        ok &= Check(defaults.enabled&&defaults.protectMedia&&defaults.protectSound&&defaults.protectFullscreen&&defaults.idleMinutes==2&&defaults.iconDesign==4&&defaults.offMinutes==0,L"settings defaults match the shipped profile: design 4 is the default icon, displays-off is opt-in");
        // The real damaged file: BOM, then two [Settings] sections. The LAST one wins - it is
        // the one the profile API actually read, so nothing the user chose is lost.
        const char* damaged="\xEF\xBB\xBF[Settings]\r\nEnabled=1\r\nProtectMedia=1\r\nProtectFullscreen=1\r\nIdleMinutes=2\r\nIcon=4\r\n[Settings]\r\nEnabled=0\r\nProtectMedia=0\r\nProtectFullscreen=1\r\nIdleMinutes=1\r\nIcon=4\r\n";
        Settings healed=ParseSettings(damaged);
        ok &= Check(!healed.enabled,L"BOM'd duplicate file: last section's Enabled wins");
        ok &= Check(!healed.protectMedia&&healed.protectFullscreen,L"BOM'd duplicate file: last section's protections win");
        ok &= Check(healed.idleMinutes==1&&healed.iconDesign==4,L"BOM'd duplicate file: last section's numbers win");
        // A BOM on its own must not hide the section, and LF endings must still parse.
        Settings bom=ParseSettings("\xEF\xBB\xBF[Settings]\nEnabled=0\nIdleMinutes=7\n");
        ok &= Check(!bom.enabled&&bom.idleMinutes==7&&bom.protectMedia,L"BOM is skipped and LF endings parse");
        // A key that is absent keeps its default rather than becoming zero.
        Settings absent=ParseSettings("[Settings]\nIcon=2\n");
        ok &= Check(absent.enabled&&absent.protectMedia&&absent.protectSound&&absent.protectFullscreen,L"absent booleans keep their defaults");
        // The Sound switch is independent of the Media switch: turning one off must not silently
        // turn the other one off, because they gate different signals.
        Settings soundOff=ParseSettings("[Settings]\r\nProtectSound=0\r\n");
        ok &= Check(!soundOff.protectSound&&soundOff.protectMedia,L"ProtectSound=0 leaves ProtectMedia alone");
        ok &= Check(ParseSettings("[Settings]\r\nProtectMedia=0\r\n").protectSound,L"ProtectMedia=0 leaves ProtectSound alone");
        ok &= Check(absent.idleMinutes==2&&absent.iconDesign==2,L"absent IdleMinutes defaults; present Icon applies");
        // An unparseable value keeps its default too.
        Settings junk=ParseSettings("[Settings]\r\nEnabled=yes\r\nIdleMinutes=\r\nIcon=3x\r\n");
        ok &= Check(junk.enabled&&junk.idleMinutes==2&&junk.iconDesign==4,L"unparseable values keep their defaults");
        // Keys outside [Settings], and sections other than it, are ignored.
        Settings outside=ParseSettings("[Other]\r\nEnabled=0\r\n[Settings]\r\nIdleMinutes=4\r\n[Other2]\r\nIdleMinutes=9\r\n");
        ok &= Check(outside.enabled&&outside.idleMinutes==4,L"keys outside [Settings] are ignored");
        // The clamps still apply on the way in.
        ok &= Check(ParseSettings("[Settings]\r\nIdleMinutes=0\r\nIcon=99\r\n").idleMinutes==1,L"parsed IdleMinutes is clamped");
        ok &= Check(ParseSettings("[Settings]\r\nIdleMinutes=0\r\nIcon=99\r\n").iconDesign==kIconDesignCount,L"parsed Icon is clamped");
        // The writer: no BOM, one section, CRLF throughout, and a round-trip through the reader.
        char canonical[256];
        int n=FormatSettings(healed,canonical,sizeof(canonical));
        ok &= Check(n>0&&(unsigned char)canonical[0]!=0xEF,L"canonical text has no BOM");
        int headers=0;for(const char* c=canonical;*c;c++)if(*c=='[')headers++;
        ok &= Check(headers==1,L"canonical text has exactly one section");
        int lineFeeds=0,bareLineFeeds=0;
        for(const char* c=canonical;*c;c++)if(*c=='\n'){lineFeeds++;if(c==canonical||c[-1]!='\r')bareLineFeeds++;}
        // Ten keys + the section header = eleven CRLF line breaks.
        ok &= Check(lineFeeds==11&&bareLineFeeds==0,L"canonical text is CRLF throughout");
        ok &= Check(SettingsTextIsCanonical(canonical),L"canonical text is recognised as canonical");
        Settings roundTrip=ParseSettings(canonical);
        ok &= Check(roundTrip.enabled==healed.enabled&&roundTrip.protectMedia==healed.protectMedia&&roundTrip.protectSound==healed.protectSound&&roundTrip.protectFullscreen==healed.protectFullscreen&&roundTrip.idleMinutes==healed.idleMinutes&&roundTrip.iconDesign==healed.iconDesign&&roundTrip.offMinutes==healed.offMinutes&&roundTrip.offInsteadOfDim==healed.offInsteadOfDim,L"canonical text round-trips through the parser");
        // Displays-off: clamped like the idle threshold, and 0 must survive the round trip as
        // "never" rather than being turned into a minimum of one minute.
        ok &= Check(ClampOffMinutes(-5)==0&&ClampOffMinutes(0)==0&&ClampOffMinutes(7)==7&&ClampOffMinutes(61)==60,L"OffMinutes is clamped to 0..60");
        Settings off=ParseSettings("[Settings]\r\nOffMinutes=30\r\n");
        ok &= Check(off.offMinutes==30,L"OffMinutes parses");
        ok &= Check(ParseSettings("[Settings]\r\nOffMinutes=999\r\n").offMinutes==60,L"parsed OffMinutes is clamped");
        ok &= Check(ParseSettings("[Settings]\r\nOffMinutes=abc\r\n").offMinutes==0&&ParseSettings("[Settings]\r\n").offMinutes==0,L"an absent or unparseable OffMinutes is 'never', not a minimum");
        char offText[256];int offLen=FormatSettings(off,offText,sizeof(offText));
        // Writer + canonical check + reader in one assertion: the formatter emits the key, the
        // canonical test accepts its own output, and the value survives.
        ok &= Check(offLen>0&&SettingsTextIsCanonical(offText)&&ParseSettings(offText).offMinutes==30,L"OffMinutes is written canonically and reads back");
        // "Off instead of dim" is a flag, not a delay: absent or unparseable means off, and any
        // non-zero value means on, so a hand-edited file cannot half-enable it.
        ok &= Check(!ParseSettings("[Settings]\r\n").offInsteadOfDim,L"OffInsteadOfDim defaults to off");
        ok &= Check(ParseSettings("[Settings]\r\nOffInsteadOfDim=1\r\n").offInsteadOfDim,L"OffInsteadOfDim=1 parses");
        ok &= Check(!ParseSettings("[Settings]\r\nOffInsteadOfDim=0\r\n").offInsteadOfDim,L"OffInsteadOfDim=0 parses as off");
        ok &= Check(!ParseSettings("[Settings]\r\nOffInsteadOfDim=abc\r\n").offInsteadOfDim,L"an unparseable OffInsteadOfDim means off");
        Settings offMode=ParseSettings("[Settings]\r\nOffInsteadOfDim=1\r\nOffMinutes=9\r\n");
        char offModeText[256];int offModeLen=FormatSettings(offMode,offModeText,sizeof(offModeText));
        Settings offModeBack=ParseSettings(offModeText);
        ok &= Check(offModeLen>0&&SettingsTextIsCanonical(offModeText)&&offModeBack.offInsteadOfDim&&offModeBack.offMinutes==9,L"OffInsteadOfDim and OffMinutes both survive the writer");
        // Acrylic: on unless the file says otherwise, and it must survive the writer.
        ok &= Check(DefaultSettings().acrylic,L"live acrylic is on by default");
        ok &= Check(!ParseSettings("[Settings]\r\nAcrylic=0\r\n").acrylic,L"Acrylic=0 parses as off");
        ok &= Check(ParseSettings("[Settings]\r\nAcrylic=1\r\n").acrylic,L"Acrylic=1 parses as on");
        ok &= Check(ParseSettings("[Settings]\r\n").acrylic,L"an absent Acrylic keeps the default");
        {
            Settings acrylicOff;
            acrylicOff.enabled=true;acrylicOff.protectMedia=true;acrylicOff.protectSound=true;
            acrylicOff.protectFullscreen=true;acrylicOff.idleMinutes=5;acrylicOff.iconDesign=4;
            acrylicOff.offMinutes=0;acrylicOff.offInsteadOfDim=false;acrylicOff.acrylic=false;
            char acrylicText[256];int acrylicLen=FormatSettings(acrylicOff,acrylicText,sizeof(acrylicText));
            ok &= Check(acrylicLen>0&&!ParseSettings(acrylicText).acrylic,L"Acrylic=0 round-trips through the writer");
        }
        // CurrentSettings() must carry EVERY field. It writes the file on every toggle, so a member
        // left unassigned does not fail to compile - it writes an indeterminate stack value, which
        // is how the startup heal once turned a user's OffMinutes into 411.
        {
            DWORD savedOffMinutes=offMinutes;
            bool savedOffInsteadOfDim=offInsteadOfDim;
            bool savedProtectSound=protectSound;
            offMinutes=17;
            offInsteadOfDim=true;
            protectSound=false;
            Settings current=CurrentSettings();
            ok &= Check(current.offMinutes==17&&current.offInsteadOfDim&&!current.protectSound,L"CurrentSettings carries every field (an unset member heals garbage)");
            offMinutes=savedOffMinutes;
            offInsteadOfDim=savedOffInsteadOfDim;
            protectSound=savedProtectSound;
        }
        ok &= Check(!SettingsTextIsCanonical(damaged),L"the BOM'd duplicate file is not canonical");
        ok &= Check(!SettingsTextIsCanonical(""),L"an absent file is not canonical");
        ok &= Check(FormatSettings(healed,canonical,8)==0,L"an undersized buffer is refused, not truncated");
    }
    // Descent buckets must span 0..kDescentSteps-1, never wrap past the end, and never
    // divide by a zero threshold (IdleMinutes is clamped to >=1, but a zero here would
    // be a silent integer-division fault on the tray path).
    ok &= DescentBucket(0,120000)==0 && DescentBucket(120000,120000)==kDescentSteps-1;
    ok &= DescentBucket(60000,120000)==kDescentSteps/2 && DescentBucket(600000,120000)==kDescentSteps-1;
    ok &= DescentBucket(0,0)==0 && DescentBucket(60000,0)==0;
    DWORD wrappedIdle=(DWORD)(50u-0xfffffff0u);
    ok &= DescentBucket(wrappedIdle,120000)==DescentBucket(66,120000);
    ok &= FlyoutLayoutSelfTest();
    ok &= SessionStatsChecks();
    ok &= ScreenGuardChecks();
    ok &= ScheduleChecks();
    ok &= PersistedStatsChecks();
    ok &= AccentChecks();
    ok &= NativeOverlayChecks();
    int powerChecks=0;
    ok &= displaypower::Checks(&powerChecks);
    int ddcChecks=0;
    ok &= ddc::Checks(&ddcChecks);
    int audioChecks=0;
    ok &= audio::Checks(&audioChecks);
    int acrylicChecks=0;
    ok &= acrylic::Checks(&acrylicChecks);
    FILE* f=_wfopen(reportFile,L"w, ccs=UTF-8");
    if(f){
        fwprintf(f,L"Decision tests: %ls (%d checks)\n",ok?L"PASS":L"FAIL",kTrayCheckCount+flyoutChecks+ddcChecks+audioChecks+acrylicChecks+powerChecks);
        // A bare FAIL across ~200 assertions is unactionable, so name the first one that
        // failed. This is how the slider log-scale bug was pinned down.
        if(!ok&&flyoutFirstFailure[0])fwprintf(f,L"First layout failure: %ls\n",flyoutFirstFailure);
        fclose(f);
    }
    return ok?0:1;
}
// --acrylic-test: run the real capture/blur/tint pipeline once and report what it did.
//
// This exists because the panel is excluded from screen capture while it is open (that exclusion is
// what lets it read what is behind itself). So the panel cannot be screenshotted to prove the blur
// works - the pipeline's own output can. It picks the busiest region of the desktop, because
// blurring a flat wallpaper proves nothing either way.
static int AcrylicTest() {
    const int W = 360, H = 520;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT best = { vx + 40, vy + 40, vx + 40 + W, vy + 40 + H };
    if (vw > W && vh > H) {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = vw;
        bi.bmiHeader.biHeight = -vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            SelectObject(mem, bmp);
            BitBlt(mem, 0, 0, vw, vh, screen, vx, vy, SRCCOPY | CAPTUREBLT);            // 128 px blocks of horizontal luminance steps; pick the window that contains the most.
            const int block = 128;
            const int bx = (vw + block - 1) / block, by = (vh + block - 1) / block;
            // Fixed bound rather than std::vector: this project avoids STL, and a 96x48 grid covers
            // any virtual screen up to 12288x6144.
            const int kMaxX = 96, kMaxY = 48;
            if (bx <= kMaxX && by <= kMaxY) {
                double energy[kMaxX * kMaxY] = { 0 };
                const BYTE* pixels = (const BYTE*)bits;
                for (int y = 0; y < vh; y++) {
                    const BYTE* row = pixels + (size_t)y * vw * 4;
                    for (int x = 0; x + 1 < vw; x++) {
                        const int a = (row[x * 4 + 2] * 299 + row[x * 4 + 1] * 587 + row[x * 4 + 0] * 114) / 1000;
                        const int b = (row[(x + 1) * 4 + 2] * 299 + row[(x + 1) * 4 + 1] * 587 + row[(x + 1) * 4 + 0] * 114) / 1000;
                        energy[(size_t)(y / block) * bx + (x / block)] += (a > b ? a - b : b - a);
                    }
                }
                double bestScore = -1;
                const int spanX = W / block, spanY = H / block;
                for (int by0 = 0; by0 + spanY <= by; by0++) {
                    for (int bx0 = 0; bx0 + spanX <= bx; bx0++) {
                        double sum = 0;
                        for (int j = 0; j < spanY; j++)
                            for (int i = 0; i < spanX; i++) sum += energy[(size_t)(by0 + j) * bx + (bx0 + i)];
                        if (sum > bestScore) {
                            bestScore = sum;
                            best = RECT{ vx + bx0 * block, vy + by0 * block,
                                         vx + bx0 * block + W, vy + by0 * block + H };
                        }
                    }
                }
            }
        }
        if (bmp) { SelectObject(mem, nullptr); DeleteObject(bmp); }
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }

    wchar_t artifact[MAX_PATH];
    swprintf(artifact, MAX_PATH, L"%ls\\acrylic-sample.bmp", folder);
    acrylic::ProbeStats st = {};
    const bool ok = acrylic::Probe(best, W, H, artifact, &st);
    FILE* f = _wfopen(reportFile, L"w, ccs=UTF-8");
    if (!f) return 2;
    fwprintf(f, L"Acrylic pipeline test: %ls\nRegion: %ld,%ld  %dx%d\nSharp  : fine %.2f  coarse %.2f  mean %.1f\nBlurred: fine %.2f  coarse %.2f  mean %.1f\nFine detail retained: %.0f%%\nCoarse structure retained: %.0f%%\nArtifact: %ls\n",
             ok ? L"PASS" : L"FAIL", best.left, best.top, W, H,
             st.fineBefore, st.coarseBefore, st.meanBefore,
             st.fineAfter, st.coarseAfter, st.meanAfter,
             st.fineBefore > 0.01 ? st.fineAfter * 100.0 / st.fineBefore : 0.0,
             st.coarseBefore > 0.01 ? st.coarseAfter * 100.0 / st.coarseBefore : 0.0,
             ok ? artifact : L"(not written)");
    fclose(f);
    // The point of a blur: kill fine detail, keep the big shapes. A flat fill would drop both.
    return (ok && st.fineBefore > 0.01 && st.fineAfter < st.fineBefore * 0.5 && st.coarseAfter > st.fineAfter) ? 0 : 1;
}

static int Diagnose() {
    // Both signals are measured, not derived: a diagnostic must be able to show the exact case
    // that motivated the fallback (SMTC empty while the audio stack is rendering).
    int session=MediaPlaying();
    // A diagnostic-only bounded startup wait; normal operation uses the 30-second timer.
    for(int i=0;i<100&&session<0&&SUCCEEDED(mediaError);i++){Sleep(20);session=MediaPlaying();}
    HRESULT audioHr=S_OK;int active=0,total=0;
    int audioState=audio::AnyActive(&audioHr,&active,&total);
    int playing=session!=0?session:audioState;
    Saved probe={};HRESULT hr=Capture(probe);
    // DDC/CI is the path that exists on a desktop, so a diagnostic that cannot see it would
    // keep reporting a machine as undimmable when it dims perfectly well. Measured values only.
    ddc::Monitor monitors[ddc::kMaxMonitors];
    int ddcCount=ddc::Enumerate(monitors,ddc::kMaxMonitors);
    FILE* f=_wfopen(reportFile,L"w, ccs=UTF-8");if(!f)return 2;
    fwprintf(f,L"Media: %ls\nMedia HRESULT: 0x%08lX\nBrightness HRESULT: 0x%08lX\nInternal panels: %lu\n",playing==1?L"playing":playing==2?L"no session, audio rendering":playing==0?L"not playing":L"unavailable",(unsigned long)mediaError,(unsigned long)hr,probe.count);
    // The two signals are reported apart because they fail apart: an empty SMTC list with audio
    // rendering is exactly the browser-with-media-keys-off case the fallback exists for.
    fwprintf(f,L"Media session (SMTC): %ls\nAudio playback (WASAPI): %ls\nAudio sessions: %d active of %d\nAudio HRESULT: 0x%08lX\n",session==1?L"playing":session==0?L"not playing":L"unavailable",audioState==1?L"playing":audioState==0?L"not playing":L"unavailable",active,total,(unsigned long)audioHr);
    fwprintf(f,L"Protections: media %ls, sound %ls, full screen %ls, off at idle %ls\n",protectMedia?L"on":L"off",protectSound?L"on":L"off",protectFullscreen?L"on":L"off",offInsteadOfDim?L"on":L"off");
    for(DWORD i=0;i<probe.count;i++)fwprintf(f,L"Panel %lu brightness: %u%%\n",i+1,probe.panels[i].brightness);
    fwprintf(f,L"DDC/CI library: %ls\nDDC monitors: %d\n",ddc::Available()?L"dxva2 loaded":L"dxva2 missing",ddcCount);
    for(int i=0;i<ddcCount;i++)fwprintf(f,L"Monitor %d \"%ls\": %lu%% (range %lu..%lu)\n",i+1,monitors[i].desc,(unsigned long)monitors[i].current,(unsigned long)monitors[i].min,(unsigned long)monitors[i].max);
    fwprintf(f,L"Dim path: %ls\n",(probe.count&&ddcCount)?L"WMI internal panel + DDC/CI external monitors":probe.count?L"WMI internal panel only":ddcCount?L"DDC/CI external monitors only":L"none");
    bool light=TaskbarIsLight();int px=TrayIconSize();
    HICON iconProbe=LoadStateIcon(iconDesign,light,px,iconDesign==kDescentDesign?kDescentPreview:L"on");bool iconOk=iconProbe!=nullptr;if(iconProbe)DestroyIcon(iconProbe);
    fwprintf(f,L"Accent resolved: #%06X   (source: %ls)\n",SystemAccent(),g_accentSource);
    fwprintf(f,L"Accent highlight: #%06X  (tile hover swatch; 0 = derived lift)\n",SystemAccentHover());
    wchar_t offText[64];
    if(offInsteadOfDim)wcscpy(offText,L"at the idle threshold, no dim");
    else if(offMinutes)swprintf(offText,64,L"%u minute(s) after dimming",offMinutes);
    else wcscpy(offText,L"never (dim only)");
    wchar_t acrylicText2[48];
    if(!acrylicEnabled)wcscpy(acrylicText2,L"off (flat plate)");
    else if(acrylicLiveEnabled)wcscpy(acrylicText2,L"live (refreshes while open; excluded from screenshots)");
    else wcscpy(acrylicText2,L"on (snapshot at open; screenshots work)");
    fwprintf(f,L"Full-screen protection active now: %ls\nAutostart: %ls\nIdle threshold: %u minutes\nDisplays off: %ls\nLive acrylic: %ls\nTray icon design: %d; theme variant: %ls; icon file: %ls; icon size: %d px\n",Fullscreen()?L"yes":L"no",Autostart()?L"on":L"off",idleMs/60000,offText,acrylicText2,iconDesign,light?L"onlight":L"ondark",iconOk?L"present":L"MISSING",px);
    if(iconDesign==kDescentDesign){
        LASTINPUTINFO input={sizeof(input),0};
        DWORD idle=GetLastInputInfo(&input)?GetTickCount()-input.dwTime:0;
        fwprintf(f,L"Live descent: idle %u s -> frame d%d of %d\n",idle/1000,DescentBucket(idle,idleMs),kDescentSteps);
    }
    fclose(f);int rc=(FAILED(hr)&&!ddcCount)||playing<0?1:0;
    return rc;
}
// The process must declare real DPI awareness before any window or DC exists, otherwise Windows
// virtualizes it at 96 DPI and the taskbar gets a 16 px icon that DWM stretches 1.5x (blurry).
// PMv2 via GetProcAddress (Win10 1703+), system-aware fallback for anything older.
typedef BOOL(WINAPI* SetDpiAwarenessContextFn)(void*);
static void DeclareDpiAwareness() {
    HMODULE user=GetModuleHandleW(L"user32.dll");
    SetDpiAwarenessContextFn setContext=user?(SetDpiAwarenessContextFn)GetProcAddress(user,"SetProcessDpiAwarenessContext"):nullptr;
    if(!setContext||!setContext((void*)-4))SetProcessDPIAware(); // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,wchar_t* command,int) {
    HDESK desk=OpenDesktopW(L"Default",0,FALSE,GENERIC_ALL);
    if(desk)SetThreadDesktop(desk);
    DeclareDpiAwareness();
    GetModuleFileNameW(nullptr,exe,MAX_PATH);wcscpy(folder,exe);wchar_t* slash=wcsrchr(folder,L'\\');if(slash)*slash=0;
    swprintf(ini,MAX_PATH,L"%ls\\settings.ini",folder);swprintf(stateFile,MAX_PATH,L"%ls\\brightness-recovery.bin",folder);swprintf(statsFile,MAX_PATH,L"%ls\\stats.bin",folder);swprintf(ddcFile,MAX_PATH,L"%ls\\ddc-recovery.bin",folder);
    swprintf(reportFile,MAX_PATH,L"%ls\\diagnostics.txt",folder);swprintf(logFile,MAX_PATH,L"%ls\\activity.log",folder);
    // Parse settings.ini once, early: diagnostic commands report the threshold before anything
    // else runs. Read-only here - the healing write happens further down and only on the normal
    // app path, so --self-test and --diagnose never touch the user's file.
    char settingsText[8192];
    bool settingsPresent=ReadSettingsText(settingsText,sizeof(settingsText));
    Settings loaded=settingsPresent?ParseSettings(settingsText):DefaultSettings();
    idleMs=ClampIdleMinutes((long)loaded.idleMinutes)*60000;
    iconDesign=ClampIconDesign((long)loaded.iconDesign);
    // Assigned here with idleMs/iconDesign, before the CLI dispatch, so --diagnose reports the
    // configured delay rather than the zero it would read from a not-yet-populated global.
    offMinutes=ClampOffMinutes((long)loaded.offMinutes);
    offInsteadOfDim=loaded.offInsteadOfDim;
    acrylicEnabled=loaded.acrylic;
    acrylicLiveEnabled=loaded.acrylicLive;
    if(wcsstr(command,L"--quit")){HWND other=FindWindowW(kClass,nullptr);if(other)PostMessageW(other,kQuit,0,0);return 0;}
    if(wcsstr(command,L"--status")){HWND other=FindWindowW(kClass,nullptr);if(other)SendMessageTimeoutW(other,kReport,0,0,SMTO_ABORTIFHUNG,5000,nullptr);return other?0:1;}
    // --menu keeps its name because scripts, README and muscle memory already use it, but it
    // now opens the Quick Settings panel - the panel is the only UI, so "open the menu" and
    // "open the panel" are the same request. It posts rather than sends, so the running
    // instance decides where the panel goes rather than the caller.
    if(wcsstr(command,L"--menu")){HWND other=FindWindowW(kClass,nullptr);if(other)PostMessageW(other,kShowPanel,0,0);return other?0:1;}
    if(wcsstr(command,L"--panel-shot")){HWND panel=FindWindowW(kPanelClass,nullptr);if(panel)SendMessageTimeoutW(panel,kPanelShot,0,0,SMTO_ABORTIFHUNG,8000,nullptr);return panel?0:1;}
    if(wcsstr(command,L"--acrylic-pause")){HWND panel=FindWindowW(kPanelClass,nullptr);if(panel)SendMessageTimeoutW(panel,kAcrylicPause,0,0,SMTO_ABORTIFHUNG,5000,nullptr);return panel?0:1;}
    if(wcsstr(command,L"--self-test"))return SelfTest();
    if(wcsstr(command,L"--acrylic-test"))return AcrylicTest();
    HRESULT init=RoInitialize(RO_INIT_SINGLETHREADED);if(FAILED(init))return 2;
    CoInitializeSecurity(nullptr,-1,nullptr,nullptr,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
    if(wcsstr(command,L"--diagnose")){
        int r=Diagnose();
        media.reset();
        mediaOperation.reset();
        RoUninitialize();return r;
    }
    HANDLE single=CreateMutexW(nullptr,TRUE,L"Local\\IdleDimmerNative");
    if(!single||GetLastError()==ERROR_ALREADY_EXISTS){if(wcsstr(command,L"--enable")){HWND other=FindWindowW(kClass,nullptr);if(other)PostMessageW(other,kEnable,0,0);}if(single)CloseHandle(single);RoUninitialize();return 0;}
    enabled=loaded.enabled;
    if(wcsstr(command,L"--enable"))enabled=true;
    protectMedia=loaded.protectMedia;
    protectSound=loaded.protectSound;
    protectFullscreen=loaded.protectFullscreen;
    LoadStats();
    // Heal a non-canonical settings.ini - a UTF-8 BOM, more than one [Settings] section, LF
    // endings, reordered keys - using exactly the values just read, once per launch. This is
    // the first point that is the normal app path: --self-test, --diagnose, --status, --quit
    // and --menu all returned above and this write never runs for them.
    if(!SettingsTextIsCanonical(settingsText))WriteSettingsFile(loaded);
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW cls={};cls.lpfnWndProc=WndProc;cls.hInstance=instance;cls.lpszClassName=kClass;cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassW(&cls);
    window=CreateWindowExW(WS_EX_TOOLWINDOW,kClass,L"Idle Dimmer",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    if(!window){CloseHandle(single);RoUninitialize();return 2;}
    LoadIcons(); // File-based themed icons; falls back to the procedural design if the files are absent.
    AddTray();WTSRegisterSessionNotification(window,NOTIFY_FOR_THIS_SESSION);
    // Crash/kill recovery: a dimmed process that died left its saved levels on disk. Either
    // mechanism's file is enough - Restore() reapplies whichever was written.
    bool recovered=LoadBrightness();
    if(LoadDdcBrightness())recovered=true;
    if(recovered&&!Restore()){enabled=false;ScheduleNext();}
    // No startup media call, deliberately. InitMedia() starts an async WinRT operation that only
    // MediaPlaying() collects, so calling it here left a request outstanding for the whole life of
    // the process whenever media protection was off - and releasing a never-completed operation is
    // what faulted inside MediaControl.dll across a suspend. Media is initialised lazily, by
    // MediaPlaying() itself. See IMPLEMENTATION.md s7.1.
    // The cadence is adaptive, not 30-second. It used to say "30-second checks" and that string is a
    // searchable literal in activity.log from every session before 2026-10-17, so entries already on
    // disk keep the old wording - which is correct for the build that wrote them. A log line is a
    // record of what a process did, not a live status; rewriting history would be the lie.
    ApplyEnabled();Log(L"Native app started; adaptive checks; input notifications only while dimmed");
    if(wcsstr(command,L"--test-brightness")){testExit=true;PostMessageW(window,WM_COMMAND,kDimTest,0);}
    // First run only: the balloon is a one-time introduction, and showing it on every launch puts a
    // notification on top of the panel in every screenshot the user takes of it.
    if(!wcsstr(command,L"--autostart")&&!settingsPresent)Balloon(L"Idle Dimmer is in your tray",L"Click the tray icon to set the idle time, media and full-screen protection, tray icon design, Start with Windows, or Quit.");
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    media.reset();
    mediaOperation.reset();
    DropDescentIcon();DestroyIcon(iconOn);DestroyIcon(iconOff);DestroyIcon(iconDim);ReleaseMutex(single);CloseHandle(single);RoUninitialize();return 0;
}
