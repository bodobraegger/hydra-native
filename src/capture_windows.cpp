// The Windows backend of src/capture.h with Windows.Graphics.Capture (Windows 10 1903 or newer), in C++/WinRT.
// initScreen uses the system picker GraphicsCapturePicker; monitor and window capture create their capture item
// with IGraphicsCaptureItemInterop, and the area is a crop of the monitor that contains its top left corner.
// The frames (BGRA textures of Direct3D 11) are copied to the CPU and scaled with nearest-neighbor sampling to
// RGBA of width x height. The helper works in physical pixels (per-monitor DPI awareness).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h> // before the C++/WinRT headers, for their support of classic COM interfaces

#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl_core.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

extern "C" {
#include "capture.h"
#include "process.h"
}

using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCapturePicker;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {

constexpr int FRAME_BUFFER_COUNT = 2;
constexpr UINT WM_PICKED = WM_APP + 1;
constexpr DirectXPixelFormat FRAME_FORMAT = DirectXPixelFormat::B8G8R8A8UIntNormalized;

struct crop {
    int x, y, width, height; // in the content of the capture item; width 0: all of it
};

struct capture_state {
    int output;
    int width, height;
    crop area;
    std::vector<uint8_t> frame;
    winrt::com_ptr<ID3D11Device> d3d_device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice device{ nullptr };
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    SizeInt32 pool_size{};
    std::mutex lock;
} state;

DWORD main_thread;

// Ends the helper at once from any thread; the parent reads the end of the stream as the end of the source.
[[noreturn]] void finish(int status) {
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
    std::abort();
}

void create_device() {
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, state.d3d_device.put(), nullptr, state.context.put()));
    auto dxgi_device = state.d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    state.device = inspectable.as<IDirect3DDevice>();
}

GraphicsCaptureItem item_for_monitor(HMONITOR monitor) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                                   winrt::put_abi(item)));
    return item;
}

GraphicsCaptureItem item_for_window(HWND window) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                                  winrt::put_abi(item)));
    return item;
}

BOOL CALLBACK add_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM list) {
    reinterpret_cast<std::vector<HMONITOR> *>(list)->push_back(monitor);
    return TRUE;
}

// A hidden window that owns the picker dialog (IInitializeWithWindow); it is never shown.
HWND create_owner_window() {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"hydra-native-capture";
    RegisterClassW(&window_class);
    return CreateWindowExW(WS_EX_TOOLWINDOW, window_class.lpszClassName, L"hydra-native capture", WS_POPUP, 0, 0, 0, 0, nullptr,
                           nullptr, window_class.hInstance, nullptr);
}

GraphicsCaptureItem pick_item() {
    GraphicsCapturePicker picker;
    picker.as<IInitializeWithWindow>()->Initialize(create_owner_window());
    auto operation = picker.PickSingleItemAsync();
    operation.Completed([](auto const &, winrt::Windows::Foundation::AsyncStatus) { PostThreadMessageW(main_thread, WM_PICKED, 0, 0); });
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0 && message.message != WM_PICKED) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return operation.GetResults(); // null when the user closed the picker
}

// Copies the content (or the crop of it) of `texture` into state.frame: nearest-neighbor scaling, BGRA to RGBA.
// False for an empty frame (a minimized window).
bool convert_frame(ID3D11Texture2D *texture, SizeInt32 content) {
    D3D11_TEXTURE2D_DESC description;
    texture->GetDesc(&description);
    int content_width = std::min(content.Width, static_cast<int>(description.Width));
    int content_height = std::min(content.Height, static_cast<int>(description.Height));
    if (content_width <= 0 || content_height <= 0) return false;
    D3D11_TEXTURE2D_DESC staging_description{};
    if (state.staging) state.staging->GetDesc(&staging_description);
    if (!state.staging || staging_description.Width != description.Width || staging_description.Height != description.Height) {
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        state.staging = nullptr;
        winrt::check_hresult(state.d3d_device->CreateTexture2D(&description, nullptr, state.staging.put()));
    }
    state.context->CopyResource(state.staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    winrt::check_hresult(state.context->Map(state.staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    crop area = state.area.width > 0 ? state.area : crop{ 0, 0, content_width, content_height };
    area.x = std::clamp(area.x, 0, content_width - 1);
    area.y = std::clamp(area.y, 0, content_height - 1);
    area.width = std::clamp(area.width, 1, content_width - area.x);
    area.height = std::clamp(area.height, 1, content_height - area.y);
    const auto *pixels = static_cast<const uint8_t *>(mapped.pData);
    uint8_t *out = state.frame.data();
    for (int y = 0; y < state.height; y++) {
        const uint8_t *row = pixels + static_cast<size_t>(area.y + y * area.height / state.height) * mapped.RowPitch;
        for (int x = 0; x < state.width; x++, out += 4) {
            const uint8_t *pixel = row + static_cast<size_t>(area.x + x * area.width / state.width) * 4;
            out[0] = pixel[2];
            out[1] = pixel[1];
            out[2] = pixel[0];
            out[3] = 255;
        }
    }
    state.context->Unmap(state.staging.get(), 0);
    return true;
}

// Runs on a thread of the thread pool (a free-threaded frame pool).
void frame_arrived(Direct3D11CaptureFramePool const &pool, winrt::Windows::Foundation::IInspectable const &) {
    std::lock_guard<std::mutex> guard(state.lock);
    try {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        SizeInt32 content = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        bool converted = convert_frame(texture.get(), content);
        frame.Close();
        // A window changed its size: the pool follows, so the next frames hold all of the window.
        if (content.Width != state.pool_size.Width || content.Height != state.pool_size.Height) {
            state.pool_size = content;
            if (converted) pool.Recreate(state.device, FRAME_FORMAT, FRAME_BUFFER_COUNT, content);
        }
        if (!converted) return;
    } catch (winrt::hresult_error const &error) {
        fprintf(stderr, "capture: %ls (0x%08x)\n", error.message().c_str(), static_cast<unsigned>(error.code()));
        finish(1);
    }
    if (!stream_write_all(state.output, state.frame.data(), state.frame.size())) finish(0);
}

void start_capture(GraphicsCaptureItem const &item) {
    state.item = item;
    state.pool_size = item.Size();
    state.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(state.device, FRAME_FORMAT, FRAME_BUFFER_COUNT, state.pool_size);
    state.pool.FrameArrived(frame_arrived);
    state.item.Closed([](auto const &, auto const &) { finish(0); });
    state.session = state.pool.CreateCaptureSession(state.item);
    state.session.StartCapture();
}

GraphicsCaptureItem find_item(const struct capture_request *request) {
    switch (request->source) {
    case CAPTURE_PICKER: return pick_item();
    case CAPTURE_MONITOR: {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&monitors));
        if (request->monitor < 0 || static_cast<size_t>(request->monitor) >= monitors.size()) {
            fprintf(stderr, "capture: no monitor %d, there are %zu\n", request->monitor, monitors.size());
            return nullptr;
        }
        return item_for_monitor(monitors[static_cast<size_t>(request->monitor)]);
    }
    case CAPTURE_AREA: {
        POINT corner{ request->x, request->y };
        HMONITOR monitor = MonitorFromPoint(corner, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof info };
        GetMonitorInfoW(monitor, &info);
        state.area = crop{ request->x - info.rcMonitor.left, request->y - info.rcMonitor.top, request->area_width,
                           request->area_height };
        return item_for_monitor(monitor);
    }
    case CAPTURE_WINDOW: {
        Sleep(static_cast<DWORD>(request->delay_seconds * 1000));
        HWND window = GetForegroundWindow();
        if (!window) {
            fprintf(stderr, "capture: no window has the focus\n");
            return nullptr;
        }
        return item_for_window(window);
    }
    }
    return nullptr;
}

const char *source_name(enum capture_source source) {
    switch (source) {
    case CAPTURE_PICKER: return "picker";
    case CAPTURE_MONITOR: return "monitor";
    case CAPTURE_AREA: return "area";
    case CAPTURE_WINDOW: return "window";
    }
    return "?";
}

} // namespace

extern "C" int capture_run(const struct capture_request *request, int output) {
    if (request->dry_run) {
        printf("Windows.Graphics.Capture %s %dx%d\n", source_name(request->source), request->width, request->height);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    main_thread = GetCurrentThreadId();
    state.output = output;
    state.width = request->width;
    state.height = request->height;
    state.frame.resize(static_cast<size_t>(request->width) * static_cast<size_t>(request->height) * 4);
    try {
        // A single-threaded apartment with a message loop, for the picker dialog.
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (!GraphicsCaptureSession::IsSupported()) {
            fprintf(stderr, "capture: Windows.Graphics.Capture needs Windows 10 1903 or newer\n");
            return 1;
        }
        create_device();
        GraphicsCaptureItem item = find_item(request);
        if (!item) {
            if (request->source == CAPTURE_PICKER) fprintf(stderr, "capture: the picker was closed\n");
            return 1;
        }
        start_capture(item);
    } catch (winrt::hresult_error const &error) {
        fprintf(stderr, "capture: %ls (0x%08x)\n", error.message().c_str(), static_cast<unsigned>(error.code()));
        return 1;
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
