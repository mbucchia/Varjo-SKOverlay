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
#include <string>
#include <vector>

#include <stereokit.h>
#include <stereokit_ui.h>
using namespace sk;

#include "utils.h"

namespace {

    // TODO: Replace me with WinRT Capture.
    BOOL(WINAPI* DwmGetDxSharedSurface)
    (HANDLE hHandle,
     HANDLE* phSurface,
     LUID* pAdapterLuid,
     ULONG* pFmtWindow,
     ULONG* pPresentFlags,
     ULONGLONG* pWin32kUpdateId) = nullptr;

    // Hook the Varjo SDK (used by the OpenXR runtime) to keep the session as an overlay.
    void (*original_varjo_WaitSync)(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) = nullptr;
    void hooked_varjo_WaitSync(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) {
        varjo_SessionSetPriority(session, 1000);

        return original_varjo_WaitSync(session, frameInfo);
    }

    struct SKOverlay {
        struct Window {
            ~Window() {
                material_release(material);
                tex_release(texture);
            }

            HWND window = nullptr;
            std::string title;
            ComPtr<ID3D11Texture2D> sharedTexture;
            tex_t texture = nullptr;
            material_t material = nullptr;
            pose_t pose = {};
            float scale = 0.75f;
            bool32_t decorate = true;
            bool32_t minimized = false;
        };

        struct AvailableWindow {
            HWND window = nullptr;
            std::string title;
            bool32_t mirrored = false;
            bool32_t wasMirrored = false;
        };

        SKOverlay() {
            ComPtr<ID3D11DeviceContext> context;
            render_get_device(reinterpret_cast<void**>(m_device.GetAddressOf()),
                              reinterpret_cast<void**>(context.GetAddressOf()));

            m_quadMesh = mesh_find(default_id_mesh_quad);
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
                        overlay->m_availableWindows.push_back(std::move(availableWindow));

                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(this));

                countdown = 500;
            }
        }

        void handleAvailableWindowsList() {
            for (auto& availableWindow : m_availableWindows) {
                // Draw the toggles.
                ui_toggle(availableWindow.title.c_str(), availableWindow.mirrored);

                // Detect toggling a window on/off.
                if (availableWindow.mirrored != availableWindow.wasMirrored) {
                    auto it = m_windows.begin();
                    for (; it != m_windows.end(); it++) {
                        if (it->window == availableWindow.window) {
                            break;
                        }
                    }

                    if (availableWindow.mirrored && it == m_windows.end()) {
                        Window newWindow = {};
                        newWindow.window = availableWindow.window;
                        newWindow.title = availableWindow.title;
                        newWindow.pose =
                            pose_t{{0, 0, -0.5f + 0.001f * (rand() % 20)}, quat_lookat(vec3_zero, {0, 0, 1})};
                        m_windows.push_back(newWindow);
                    } else if (!availableWindow.mirrored && it != m_windows.end()) {
                        m_windows.erase(it);
                    }
                }
                availableWindow.wasMirrored = availableWindow.mirrored;
            }
        }

        void ensureWindowResources(Window& window) {
            if (!window.texture) {
                HANDLE surface = nullptr;
                LUID luid = {
                    0,
                };
                ULONG format = 0;
                ULONG flags = 0;
                ULONGLONG update_id = 0;
                if (!DwmGetDxSharedSurface(window.window, &surface, &luid, &format, &flags, &update_id)) {
                    log_warn("Failed to get surface!");
                    return;
                }

                ComPtr<ID3D11Texture2D> sharedTexture = nullptr;
                if (FAILED(m_device->OpenSharedResource(surface, IID_PPV_ARGS(sharedTexture.GetAddressOf())))) {
                    log_warn("Failed to get shared surface!");
                    return;
                }

                window.material = material_copy_id(default_id_material_unlit);
                window.sharedTexture = sharedTexture;
                window.texture = tex_create();
                tex_set_surface(window.texture, sharedTexture.Get(), tex_type_image_nomips, format, 0, 0, 1);
                tex_set_address(window.texture, tex_address_clamp);
                material_set_texture(window.material, "diffuse", window.texture);
            }
        }

        void drawWindows() {
            for (auto& window : m_windows) {
                ensureWindowResources(window);

                // Draw the window.
                ui_window_begin(
                    window.title.c_str(), window.pose, vec2_zero, window.decorate ? ui_win_head : ui_win_empty);

                if (!window.minimized) {
                    vec2 size = vec2{(float)tex_get_width(window.texture), (float)tex_get_height(window.texture)} *
                                0.0004f * window.scale;
                    ui_layout_reserve(size);

                    render_add_mesh(m_quadMesh,
                                    window.material,
                                    matrix_trs(vec3{0, -size.y / 2, 0}, quat_identity, vec3{size.x, size.y, 1}));

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
            }
        }

        void step(void) {
            ui_window_begin("Window Selection", m_menuPose);

            bool wasMinimized = m_minimized;
            if (ui_button(m_minimized ? "Open" : "Close")) {
                m_minimized = !m_minimized;
            }

            refreshAvailableWindows(!m_minimized && wasMinimized);

            if (!m_minimized) {
                handleAvailableWindowsList();
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

        bool m_minimized = false;
        pose_t m_menuPose = pose_t{{0, 0, -0.5f}, quat_lookat(vec3_zero, {0, 0, 1})};

        mesh_t m_quadMesh;
        ComPtr<ID3D11Device> m_device;

        std::vector<Window> m_windows;
        std::vector<AvailableWindow> m_availableWindows;
    };

} // namespace

int main(void) {
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

    DwmGetDxSharedSurface =
        (decltype(DwmGetDxSharedSurface))GetProcAddress(LoadLibrary("user32.dll"), "DwmGetDxSharedSurface");

    SKOverlay overlay;
    overlay.run();

    return 0;
}
