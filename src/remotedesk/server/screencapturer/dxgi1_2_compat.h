// 老 MinGW-w64 头文件补齐：DXGI 1.2 桌面复制接口。
//
// Qt 5.7 自带的 mingw-w64 3.x 的 dxgi1_2.h 只声明了 IDXGIDevice2/SwapChain1/
// Factory2/Adapter2，缺 IDXGIOutput1、IDXGIOutputDuplication 及其结构体。
// 这里按 Windows SDK 的 vtable 顺序补齐（仅在 CMake 探测失败时经
// QTRD_DXGI_COMPAT 引入）。
//
// 不需要动态 LoadLibrary，也不链接 dxguid：
//   - D3D11CreateDevice / IDXGIDevice / ID3D11Texture2D 等基础接口在老头文件里就有；
//   - 缺失的只有"接口声明"，d3d11/dxgi 的导入库（libd3d11.a / libdxgi.a）工具链自带；
//   - IID 通过 __CRT_UUID_DECL 内联（老 mingw 的 __uuidof 是 _mingw.h 的模板宏），
//     不依赖 IID_* 导入符号。
// 是否真正调用桌面复制由运行期 IsWindows8OrGreater() 决定（Win7 走 GDI）。
#ifndef QTRD_DXGI12_COMPAT_H
#define QTRD_DXGI12_COMPAT_H

#if !defined(__IDXGIOutput1_INTERFACE_DEFINED__)
#define __IDXGIOutput1_INTERFACE_DEFINED__

typedef struct DXGI_MODE_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    WINBOOL Stereo;
} DXGI_MODE_DESC1;

typedef struct DXGI_OUTDUPL_POINTER_POSITION {
    POINT Position;
    WINBOOL Visible;
} DXGI_OUTDUPL_POINTER_POSITION;

typedef struct DXGI_OUTDUPL_POINTER_SHAPE_INFO {
    UINT Type;
    UINT Width;
    UINT Height;
    UINT Pitch;
    POINT HotSpot;
} DXGI_OUTDUPL_POINTER_SHAPE_INFO;

typedef struct DXGI_OUTDUPL_FRAME_INFO {
    LARGE_INTEGER LastPresentTime;
    LARGE_INTEGER LastMouseUpdateTime;
    UINT AccumulatedFrames;
    WINBOOL RectsCoalesced;
    WINBOOL ProtectedContentMaskedOut;
    DXGI_OUTDUPL_POINTER_POSITION PointerPosition;
    UINT TotalMetadataBufferSize;
    UINT PointerShapeBufferSize;
} DXGI_OUTDUPL_FRAME_INFO;

typedef struct DXGI_OUTDUPL_DESC {
    DXGI_MODE_DESC ModeDesc;
    DXGI_MODE_ROTATION Rotation;
    WINBOOL DesktopImageInSystemMemory;
} DXGI_OUTDUPL_DESC;

typedef interface IDXGIOutputDuplication IDXGIOutputDuplication;

MIDL_INTERFACE("00cddea8-939b-4b83-a340-a685226666cc")
IDXGIOutput1 : public IDXGIOutput
{
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList1(
        DXGI_FORMAT format, UINT flags, UINT *mode_count, DXGI_MODE_DESC1 *desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(
        const DXGI_MODE_DESC1 *mode, DXGI_MODE_DESC1 *closest_match,
        IUnknown *device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(
        IDXGIResource *destination) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateOutput(
        IUnknown *device, IDXGIOutputDuplication **output_duplication) = 0;
};

MIDL_INTERFACE("191cfac3-a341-470d-b26e-a864f428319c")
IDXGIOutputDuplication : public IDXGIObject
{
    virtual void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireNextFrame(
        UINT timeout_in_milliseconds, DXGI_OUTDUPL_FRAME_INFO *frame_info,
        IDXGIResource **desktop_resource) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFramePointerShape(
        UINT shape_buffer_size, void *shape_buffer,
        UINT *shape_buffer_required_size,
        DXGI_OUTDUPL_POINTER_SHAPE_INFO *shape_info) = 0;
    virtual HRESULT STDMETHODCALLTYPE MapDesktopSurface(
        DXGI_MAPPED_RECT *locked_rect) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseFrame() = 0;
};

#if defined(__CRT_UUID_DECL)
__CRT_UUID_DECL(IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3,0x40, 0xa6,0x85, 0x22,0x66, 0x66,0xcc)
__CRT_UUID_DECL(IDXGIOutputDuplication, 0x191cfac3, 0xa341, 0x470d, 0xb2,0x6e, 0xa8,0x64, 0xf4,0x28, 0x31,0x9c)
#endif

#endif // __IDXGIOutput1_INTERFACE_DEFINED__
#endif // QTRD_DXGI12_COMPAT_H
