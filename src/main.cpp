// MIT License
//
// Copyright(c) 2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Largely inspired from sample code at
// https://github.com/StereoKit/StereoKit/blob/master/Examples/StereoKitCTest/demo_windows.cpp

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;
#include <winuser.h>
#include <d3d11.h>
#include <Varjo.h>
#include <detours.h>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

#include <stereokit.h>
#include <stereokit_ui.h>
using namespace sk;

#include "utils.h"

namespace {

    // Hook the Varjo SDK (used by the OpenXR runtime) to keep the session as an overlay.
    void (*original_varjo_WaitSync)(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) = nullptr;
    void hooked_varjo_WaitSync(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) {
        varjo_SessionSetPriority(session, 1000);

        return original_varjo_WaitSync(session, frameInfo);
    }

    // Alternative to windows.graphics.directx.direct3d11.interop.h
    extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice* dxgiDevice, ::IInspectable** graphicsDevice);

    HRESULT __stdcall CreateDirect3D11SurfaceFromDXGISurface(::IDXGISurface* dgxiSurface,
                                                             ::IInspectable** graphicsSurface);
    }

    // https://gist.github.com/kennykerr/15a62c8218254bc908de672e5ed405fa
    struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDXGIInterfaceAccess : ::IUnknown {
        virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
    };

    // Helper for WinRT window capture.
    class CaptureWindow {
      public:
        CaptureWindow(ID3D11Device* device, HWND window) {
            auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                                 IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop_factory->CreateForWindow(
                window,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(m_item)));

            initialize(device);
        }

        CaptureWindow(ID3D11Device* device, HMONITOR monitor) {
            auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                                 IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop_factory->CreateForMonitor(
                monitor,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(m_item)));

            initialize(device);
        }

        ~CaptureWindow() {
            m_session.Close();
            m_framePool.Close();
        }

        ID3D11Texture2D* getSurface() const {
            auto frame = m_framePool.TryGetNextFrame();
            if (frame != nullptr) {
                ComPtr<ID3D11Texture2D> surface;
                auto access = frame.Surface().as<IDirect3DDXGIInterfaceAccess>();
                winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(),
                                                          reinterpret_cast<void**>(surface.ReleaseAndGetAddressOf())));

                m_lastCapturedFrame = frame;
                m_lastCapturedSurface = surface;
            }

            return m_lastCapturedSurface.Get();
        }

        std::pair<int32_t, int32_t> getSize() const {
            return {m_item.Size().Width, m_item.Size().Height};
        }

      private:
        void initialize(ID3D11Device* device) {
            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));
            ComPtr<IInspectable> object;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), object.GetAddressOf()));
            winrt::check_hresult(
                object->QueryInterface(winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(),
                                       winrt::put_abi(m_interopDevice)));

            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_interopDevice,
                static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(DXGI_FORMAT_R8G8B8A8_UNORM),
                2,
                m_item.Size());
            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.StartCapture();
        }

        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_interopDevice;
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
        mutable winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_lastCapturedFrame{nullptr};
        mutable ComPtr<ID3D11Texture2D> m_lastCapturedSurface;
    };

    struct SKOverlay {
        struct Window {
            HWND window = nullptr;
            HMONITOR monitor = nullptr;
            std::string title;
            std::shared_ptr<CaptureWindow> captureWindow;
            ComPtr<ID3D11Texture2D> sharedTexture;
            tex_t texture = nullptr;
            material_t material = nullptr;
            pose_t pose = {};
            float scale = 0.75f;
            bool32_t decorate = true;
            bool32_t minimized = false;
            bool cleanup = false;
        };

        struct AvailableWindow {
            HWND window = nullptr;
            HMONITOR monitor = nullptr;
            std::string title;
            bool32_t mirrored = false;
            bool32_t wasMirrored = false;
        };

        SKOverlay() {
            ComPtr<ID3D11DeviceContext> context;
            render_get_device(reinterpret_cast<void**>(m_device.GetAddressOf()),
                              reinterpret_cast<void**>(context.GetAddressOf()));

            m_quadMesh = mesh_find(default_id_mesh_quad);

            initializeAvailableMonitors();
        }

        void initializeAvailableMonitors() {
            m_availableMonitors.clear();

            EnumDisplayMonitors(
                nullptr,
                nullptr,
                [](HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM lParam) {
                    SKOverlay* overlay = reinterpret_cast<SKOverlay*>(lParam);

                    MONITORINFOEX monitorInfo{};
                    monitorInfo.cbSize = sizeof(monitorInfo);
                    if (GetMonitorInfo(monitor, &monitorInfo)) {
                        AvailableWindow availableMonitor;
                        availableMonitor.monitor = monitor;
                        availableMonitor.title = "Monitor ";
                        availableMonitor.title += std::string(monitorInfo.szDevice);
                        overlay->m_availableMonitors.push_back(std::move(availableMonitor));
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(this));
        }

        void refreshAvailableWindows(bool forceRefresh) {
            static int countdown = 0;
            if (forceRefresh || --countdown <= 0) {
                m_availableWindows.clear();

                EnumWindows(
                    [](HWND hwnd, LPARAM lParam) {
                        SKOverlay* overlay = reinterpret_cast<SKOverlay*>(lParam);

                        if (hwnd == nullptr)
                            return TRUE;
                        if (hwnd == GetShellWindow())
                            return TRUE;
                        if (!IsWindowVisible(hwnd))
                            return TRUE;
                        if (GetAncestor(hwnd, GA_ROOT) != hwnd)
                            return TRUE;

                        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
                        if (style & WS_DISABLED)
                            return TRUE;

                        char text[256];
                        GetWindowText(hwnd, text, sizeof(text));
                        if (strcmp(text, "") == 0)
                            return TRUE;

                        AvailableWindow availableWindow;
                        availableWindow.window = hwnd;
                        availableWindow.title = text;
                        for (const auto& window : overlay->m_windows) {
                            if (window.window == hwnd) {
                                availableWindow.mirrored = availableWindow.wasMirrored = true;
                                break;
                            }
                        }
                        // Open the windows that matched filters.
                        for (const auto& filter : overlay->m_filters) {
                            if (std::regex_search(availableWindow.title, filter)) {
                                availableWindow.mirrored = true;
                            }
                        }
                        overlay->m_availableWindows.push_back(std::move(availableWindow));

                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(this));

                countdown = 500;
            }
        }

        void handleAvailableWindowsList(std::vector<AvailableWindow>& availableWindows) {
            for (auto& availableWindow : availableWindows) {
                // Draw the toggles.
                ui_toggle(availableWindow.title.c_str(), availableWindow.mirrored);

                // Detect toggling a window on/off.
                if (availableWindow.mirrored != availableWindow.wasMirrored) {
                    auto it = m_windows.begin();
                    for (; it != m_windows.end(); it++) {
                        if (it->window && it->window == availableWindow.window) {
                            break;
                        }
                        if (it->monitor && it->monitor == availableWindow.monitor) {
                            break;
                        }
                    }

                    if (availableWindow.mirrored && it == m_windows.end()) {
                        Window newWindow = {};
                        newWindow.window = availableWindow.window;
                        newWindow.monitor = availableWindow.monitor;
                        newWindow.title = availableWindow.title;
                        newWindow.pose =
                            pose_t{{0, 0, -0.5f + 0.001f * (rand() % 20)}, quat_lookat(vec3_zero, {0, 0, 1})};
                        m_windows.push_back(newWindow);
                    } else if (!availableWindow.mirrored && it != m_windows.end()) {
                        material_release(it->material);
                        tex_release(it->texture);
                        m_windows.erase(it);
                    }
                }
                availableWindow.wasMirrored = availableWindow.mirrored;
            }
        }

        void ensureWindowResources(Window& window) {
            if (window.window && !IsWindow(window.window)) {
                window.cleanup = true;
                return;
            }

            if (!window.texture) {
                try {
                    if (window.window) {
                        window.captureWindow = std::make_shared<CaptureWindow>(m_device.Get(), window.window);
                    } else {
                        window.captureWindow = std::make_shared<CaptureWindow>(m_device.Get(), window.monitor);
                    }
                } catch (...) {
                    log_warn("Failed to open window capture");
                }
                window.material = material_copy_id(default_id_material_unlit);
                window.texture = tex_create();
                tex_set_address(window.texture, tex_address_clamp);
                material_set_texture(window.material, "diffuse", window.texture);
            }
        }

        void drawWindows() {
            auto it = m_windows.begin();
            while (it != m_windows.end()) {
                auto& window = *it;

                ensureWindowResources(window);
                if (window.cleanup) {
                    material_release(it->material);
                    tex_release(it->texture);
                    it = m_windows.erase(it);
                    continue;
                }

                // Draw the window.
                ui_window_begin(window.title.c_str(),
                                window.pose,
                                vec2_zero,
                                window.decorate ? ui_win_head : ui_win_empty,
                                sk::ui_move_exact);

                if (!window.minimized) {
                    std::pair<int32_t, int32_t> size;
                    if (window.captureWindow) {
                        window.sharedTexture = window.captureWindow->getSurface();
                        if (window.sharedTexture) {
                            D3D11_TEXTURE2D_DESC desc{};
                            window.sharedTexture->GetDesc(&desc);

                            tex_set_surface(window.texture,
                                            window.sharedTexture.Get(),
                                            tex_type_image_nomips,
                                            desc.Format,
                                            0,
                                            0,
                                            1);
                        }
                        size = window.captureWindow->getSize();
                    }

                    vec2 scaledSize =
                        vec2{window.captureWindow ? (float)size.first : (float)tex_get_width(window.texture),
                             window.captureWindow ? (float)size.second : (float)tex_get_height(window.texture)} *
                        0.0004f * window.scale;
                    ui_layout_reserve(scaledSize);

                    render_add_mesh(
                        m_quadMesh,
                        window.material,
                        matrix_trs(vec3{0, -scaledSize.y / 2, 0}, quat_identity, vec3{scaledSize.x, scaledSize.y, 1}));

                    if (ui_button("+")) {
                        window.scale *= 1.1f;
                    }
                    ui_sameline();
                    if (ui_button("-")) {
                        window.scale *= 0.9f;
                    }
                    ui_sameline();
                }

                if (ui_button(window.decorate ? "Hide title" : "Show title")) {
                    window.decorate = !window.decorate;
                }
                ui_sameline();

                if (ui_button(window.minimized ? "Show" : "Minimize")) {
                    window.minimized = !window.minimized;
                }

                ui_window_end();

                it++;
            }
        }

        void step(void) {
            ui_window_begin("Window Selection", m_menuPose, vec2{0, 0}, sk::ui_win_normal, sk::ui_move_exact);

            bool wasMinimized = m_minimized;
            if (ui_button(m_minimized ? "Open" : "Close")) {
                m_minimized = !m_minimized;
            }
            ui_sameline();
            if (ui_button(m_handsVisible ? "Hide hands" : "Show hands")) {
                m_handsVisible = !m_handsVisible;
                input_hand_visible(sk::handed_left, m_handsVisible);
                input_hand_visible(sk::handed_right, m_handsVisible);
            }

            refreshAvailableWindows(!m_minimized && wasMinimized);

            if (!m_minimized) {
                ui_hseparator();
                handleAvailableWindowsList(m_availableMonitors);
                ui_hseparator();
                handleAvailableWindowsList(m_availableWindows);
            }

            ui_window_end();

            drawWindows();
        }

        void run() {
            sk_run_data(
                [](void* opaque) {
                    SKOverlay* overlay = reinterpret_cast<SKOverlay*>(opaque);
                    overlay->step();
                },
                this,
                nullptr,
                this);
        }

        void addFilter(const std::string& expression) {
            if (expression.empty()) {
                return;
            }
            m_filters.push_back(std::regex(expression, std::regex_constants::ECMAScript | std::regex_constants::icase));
        }

        bool m_minimized = false;
        pose_t m_menuPose = pose_t{{0.35f, 0, -0.35f}, quat_lookat({0.35f, 0, -0.35f}, {0, 0, 0})};

        mesh_t m_quadMesh;
        ComPtr<ID3D11Device> m_device;

        bool m_handsVisible = true;

        std::vector<Window> m_windows;
        std::vector<AvailableWindow> m_availableMonitors;
        std::vector<AvailableWindow> m_availableWindows;

        std::vector<std::regex> m_filters;
    };

} // namespace

int main(int argc, char** argv) {
    DetourRestoreAfterWith();

    sk_settings_t settings = {};
    settings.app_name = "SKOverlayApp";
    settings.assets_folder = "Assets";
    settings.display_preference = display_mode_mixedreality;
    if (!sk_init(settings))
        return 1;

    // Hook the Varjo runtime to force overlay mode.
    const std::filesystem::path varjoLib =
        std::filesystem::path(Utils::RegGetString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Varjo\\Runtime", "InstallDir")
                                  .value_or(L"C:\\Program Files\\Varjo")) /
        L"varjo-openxr" / L"VarjoLib.dll";
    Utils::DetourDllAttach(varjoLib.c_str(), "varjo_WaitSync", hooked_varjo_WaitSync, original_varjo_WaitSync);

    // Disable skybox to ensure a transparent background.
    render_enable_skytex(false);

    SKOverlay overlay;
    for (int i = 1; i < argc; i++) {
        overlay.addFilter(argv[i]);
    }
    overlay.run();

    return 0;
}
