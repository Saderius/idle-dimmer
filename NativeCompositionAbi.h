// ABI prefixes from the Microsoft Windows SDK (10.0.22621.0).
// Unused methods retain their vtable slots; they are never called. No SDK runtime is bundled.
// See Microsoft Visual Layer with Win32 documentation and windows.ui.composition*.h.
#pragma once
#include <windows.h>
#include <inspectable.h>
namespace nativebackdrop { namespace abi {
struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
inline GUID Uuid(const wchar_t* text) {
    GUID id = {};
    wchar_t braced[39]; braced[0] = L'{';
    lstrcpynW(braced + 1, text, 37); braced[37] = L'}'; braced[38] = 0;
    CLSIDFromString(braced, &id);
    return id;
}
struct IGraphicsEffect;
struct ICompositor;
struct ICompositor3;
struct ICompositionBrush;
struct ICompositionTarget;
struct ISpriteVisual;
struct IVisual;
struct IContainerVisual;
struct IVisualCollection;
struct ICompositionEffectFactory;
struct ICompositionEffectBrush;
struct ICompositionEffectSourceParameterFactory;
struct IPropertyValueStatics;
struct IDispatcherQueueController;
struct ICompositor : IInspectable {
    static GUID iid() { return Uuid(L"b403ca50-7f8c-4e83-985f-cc45060036d8"); }
    virtual HRESULT STDMETHODCALLTYPE unused_CreateColorKeyFrameAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateColorBrush() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateColorBrushWithColor() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateContainerVisual(IContainerVisual** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateCubicBezierEasingFunction() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateEffectFactory(IGraphicsEffect* effect, ICompositionEffectFactory** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateEffectFactoryWithProperties() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateExpressionAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateExpressionAnimationWithExpression() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInsetClip() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInsetClipWithInsets() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateLinearEasingFunction() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreatePropertySet() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateQuaternionKeyFrameAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateScalarKeyFrameAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateScopedBatch() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSpriteVisual(ISpriteVisual** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateSurfaceBrush() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSurfaceBrushWithSurface(IInspectable* surface, IInspectable** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateTargetForCurrentView() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateVector2KeyFrameAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateVector3KeyFrameAnimation() = 0;
};
struct ICompositor3 : IInspectable {
    static GUID iid() { return Uuid(L"c9dd8ef0-6eb1-4e3c-a658-675d9c64d4ab"); }
    virtual HRESULT STDMETHODCALLTYPE CreateHostBackdropBrush(IInspectable** result) = 0;
};
struct ICompositor5 : IInspectable {
    static GUID iid() { return Uuid(L"48ea31ad-7fcd-4076-a79c-90cc4b852c9b"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_Comment() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_Comment() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_GlobalPlaybackRate() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_GlobalPlaybackRate() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateBounceScalarAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateBounceVector2Animation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateBounceVector3Animation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateContainerShape() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateEllipseGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateLineGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreatePathGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreatePathGeometryWithPath() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreatePathKeyFrameAnimation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateRectangleGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateRoundedRectangleGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateShapeVisual() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateSpriteShape() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateSpriteShapeWithGeometry() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateViewBox() = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestCommitAsync(IInspectable**) = 0;
};
struct ICompositionBrush : IInspectable {
    static GUID iid() { return Uuid(L"ab0d7608-30c0-40e9-b568-b60a6bd1fb46"); }
};
struct ICompositionSurfaceBrush : IInspectable {
    static GUID iid() { return Uuid(L"ad016d79-1e4c-4c0d-9c29-83338c87c162"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_BitmapInterpolationMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_BitmapInterpolationMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_HorizontalAlignmentRatio() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_HorizontalAlignmentRatio(float) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Stretch() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Stretch(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Surface() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_Surface() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_VerticalAlignmentRatio() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_VerticalAlignmentRatio(float) = 0;
};
struct ICompositionTarget : IInspectable {
    static GUID iid() { return Uuid(L"a1bea8ba-d726-4663-8129-6b5e7927ffa6"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_Root() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Root(IVisual* root) = 0;
};
struct ISpriteVisual : IInspectable {
    static GUID iid() { return Uuid(L"08e05581-1ad1-4f97-9757-402d76e4233b"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_Brush() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Brush(ICompositionBrush* brush) = 0;
};
struct IVisual : IInspectable {
    static GUID iid() { return Uuid(L"117e202d-a859-4c89-873b-c2aa566788e3"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_AnchorPoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_AnchorPoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_BackfaceVisibility() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_BackfaceVisibility() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_BorderMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_BorderMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_CenterPoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_CenterPoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Clip() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_Clip() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_CompositeMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_CompositeMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_IsVisible() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_IsVisible() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Offset() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Offset(Vector3 offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Opacity() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Opacity(float opacity) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Orientation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_Orientation() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Parent() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_RotationAngle() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_RotationAngle() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_RotationAngleInDegrees() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_RotationAngleInDegrees() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_RotationAxis() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_RotationAxis() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Scale() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_put_Scale() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_get_Size() = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Size(Vector2 size) = 0;
};
struct IContainerVisual : IInspectable {
    static GUID iid() { return Uuid(L"02f6bc74-ed20-4773-afe6-d49b4a93db32"); }
    virtual HRESULT STDMETHODCALLTYPE get_Children(IVisualCollection** result) = 0;
};
struct IVisualCollection : IInspectable {
    static GUID iid() { return Uuid(L"8b745505-fd3e-4a98-84a8-e949468c6bcb"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_Count() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_InsertAbove() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_InsertAtBottom() = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAtTop(IVisual* child) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_InsertBelow() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAll() = 0;
};
struct ICompositionEffectFactory : IInspectable {
    static GUID iid() { return Uuid(L"be5624af-ba7e-4510-9850-41c0b4ff74df"); }
    virtual HRESULT STDMETHODCALLTYPE CreateBrush(ICompositionEffectBrush** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExtendedError(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LoadStatus(int*) = 0;
};
struct ICompositionEffectBrush : IInspectable {
    static GUID iid() { return Uuid(L"bf7f795e-83cc-44bf-a447-3e3c071789ec"); }
    virtual HRESULT STDMETHODCALLTYPE unused_GetSourceParameter() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSourceParameter(HSTRING name, ICompositionBrush* source) = 0;
};
struct ICompositionEffectSourceParameterFactory : IInspectable {
    static GUID iid() { return Uuid(L"b3d9f276-aba3-4724-acf3-d0397464db1c"); }
    virtual HRESULT STDMETHODCALLTYPE Create(HSTRING name, IInspectable** result) = 0;
};
struct IPropertyValueStatics : IInspectable {
    static GUID iid() { return Uuid(L"629bdbc8-d932-4ff4-96b9-8d96c5c1e858"); }
    virtual HRESULT STDMETHODCALLTYPE unused_CreateEmpty() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt8() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt16() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt16() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt32() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateUInt32(UINT32 value, IInspectable** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt64() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt64() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSingle(float value, IInspectable** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateDouble() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateChar16() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateBoolean() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateString() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInspectable() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateGuid() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateDateTime() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateTimeSpan() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreatePoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateSize() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateRect() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt8Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt16Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt16Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt32Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt32Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateInt64Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE unused_CreateUInt64Array() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSingleArray(UINT32 count, float* values, IInspectable** result) = 0;
};
struct IDispatcherQueueController : IInspectable {
    static GUID iid() { return Uuid(L"22f34e66-50db-4e36-a98d-61c01b384d20"); }
    virtual HRESULT STDMETHODCALLTYPE unused_get_DispatcherQueue() = 0;
    virtual HRESULT STDMETHODCALLTYPE ShutdownQueueAsync(IInspectable** result) = 0;
};
} } // namespace nativebackdrop::abi
