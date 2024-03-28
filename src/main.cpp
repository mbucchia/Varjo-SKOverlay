#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winuser.h>
#include <d3d11.h>
#include <Varjo.h>
#include <detours.h>
#include <vector>

#include <stereokit.h>
#include <stereokit_ui.h>
using namespace sk;

#include "utils.h"

void (*original_varjo_WaitSync)(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) = nullptr;
void hooked_varjo_WaitSync(struct varjo_Session* session, struct varjo_FrameInfo* frameInfo) {
	// Keep the session as an overlay.
	varjo_SessionSetPriority(session, 1000);

	return original_varjo_WaitSync(session, frameInfo);
}

// Largely inspired from sample code at https://github.com/StereoKit/StereoKit/blob/master/Examples/StereoKitCTest/demo_windows.cpp

mesh_t        mirror_mesh;
ID3D11Device* mirror_device;

struct window_t {
	HWND window;
	ID3D11Texture2D* shared_texture;
	tex_t texture;
	material_t material;
	pose_t pose;
	char name[128];
	float scale;
	bool decorate;
};
std::vector<window_t> mirror_windows;

struct available_window_t {
	HWND window;
	char name[128];
	bool32_t mirrored;
	bool32_t was_mirrored;
};
std::vector<available_window_t> available_windows;

// TODO: Replace me with WinRT Capture.
typedef BOOL(WINAPI* fn_GetDxSharedSurface)(HANDLE hHandle, HANDLE* phSurface, LUID* pAdapterLuid, ULONG* pFmtWindow, ULONG* pPresentFlags, ULONGLONG* pWin32kUpdateId);
fn_GetDxSharedSurface DwmGetDxSharedSurface;

void step(void) {
	static pose_t window_pose =
		pose_t{ {0,0,-0.5f}, quat_lookat(vec3_zero, {0,0,1}) };

	ui_window_begin("Window Selection", window_pose);

	static bool minimized = false;
	bool was_minimized = minimized;
	if (ui_button(minimized ? "Open" : "Close")) {
		minimized = !minimized;
	}

	// Refresh the list of available windows.
	static int countdown = 0;
	if ((!minimized && was_minimized) || --countdown <= 0) {
		available_windows.clear();

		EnumWindows([](HWND hwnd, LPARAM) {
			if (hwnd == nullptr)          return TRUE;
			if (hwnd == GetShellWindow()) return TRUE;
			if (!IsWindowVisible(hwnd))   return TRUE;
			if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;

			LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
			if (style & WS_DISABLED) return TRUE;

			char text[256];
			GetWindowText(hwnd, text, sizeof(text));
			if (strcmp(text, "") == 0) return TRUE;

			available_window_t win = {};
			win.window = hwnd;
			strncpy(win.name, text, sizeof(win.name));
			for (size_t j = 0; j < mirror_windows.size(); j++) {
				if (mirror_windows[j].window == hwnd) {
					win.mirrored = win.was_mirrored = true;
					break;
				}
			}
			available_windows.push_back(win);

			return TRUE;
			}, 0);

		countdown = 500;
	}

	if (!minimized) {
		// Draw the toggles.
		for (size_t i = 0; i < available_windows.size(); i++) {
			ui_toggle(available_windows[i].name, available_windows[i].mirrored);

			// Detect toggling a window on/off.
			if (available_windows[i].mirrored != available_windows[i].was_mirrored) {
				size_t j = 0;
				for (; j < mirror_windows.size(); j++) {
					if (mirror_windows[j].window == available_windows[i].window) {
						break;
					}
				}

				if (available_windows[i].mirrored && j == mirror_windows.size()) {
					window_t win = {};
					win.window = available_windows[i].window;
					strncpy(win.name, available_windows[i].name, sizeof(win.name));
					win.pose = pose_t{ {0,0,-0.5f + 0.001f * mirror_windows.size()}, quat_lookat(vec3_zero, {0,0,1}) };
					win.scale = 0.5f;
					win.decorate = true;
					mirror_windows.push_back(win);
				}
				else if (!available_windows[i].mirrored && j != mirror_windows.size()) {
					material_release(mirror_windows[j].material);
					if (mirror_windows[j].shared_texture) {
						mirror_windows[j].shared_texture->Release();
					}
					tex_release(mirror_windows[j].texture);
					mirror_windows.erase(mirror_windows.begin() + j);
				}
			}
			available_windows[i].was_mirrored = available_windows[i].mirrored;
		}
	}

	ui_window_end();

	for (size_t i = 0; i < mirror_windows.size(); i++) {
		// Create resources for the window.
		if (!mirror_windows[i].texture) {
			HANDLE    surface = nullptr;
			LUID      luid = { 0, };
			ULONG     format = 0;
			ULONG     flags = 0;
			ULONGLONG update_id = 0;
			if (!DwmGetDxSharedSurface(mirror_windows[i].window, &surface, &luid, &format, &flags, &update_id)) {
				log_warn("Failed to get surface!");
				continue;
			}

			ID3D11Texture2D* shared_tex = nullptr;
			if (FAILED(mirror_device->OpenSharedResource(surface, __uuidof(ID3D11Texture2D), (void**)(&shared_tex)))) {
				log_warn("Failed to get shared surface!");
				continue;
			}

			mirror_windows[i].material = material_copy_id(default_id_material_unlit);
			mirror_windows[i].shared_texture = shared_tex;
			mirror_windows[i].texture = tex_create();
			tex_set_surface(mirror_windows[i].texture, shared_tex, tex_type_image_nomips, format, 0, 0, 1);
			tex_set_address(mirror_windows[i].texture, tex_address_clamp);
			material_set_texture(mirror_windows[i].material, "diffuse", mirror_windows[i].texture);
		}

		// Draw the window.
		ui_window_begin(mirror_windows[i].name, mirror_windows[i].pose, vec2_zero, mirror_windows[i].decorate ? ui_win_head : ui_win_empty);

		vec2 size = vec2{
			(float)tex_get_width(mirror_windows[i].texture),
			(float)tex_get_height(mirror_windows[i].texture) } *0.0004f * mirror_windows[i].scale;
		ui_layout_reserve(size);

		render_add_mesh(mirror_mesh, mirror_windows[i].material, matrix_trs(vec3{ 0, -size.y / 2, 0 }, quat_identity, vec3{ size.x, size.y, 1 }));

		if (ui_button("+")) {
			mirror_windows[i].scale *= 1.1f;
		}
		ui_sameline();
		if (ui_button("-")) {
			mirror_windows[i].scale *= 0.9f;
		}

		if (ui_button(mirror_windows[i].decorate ? "Hide title" : "Show title")) {
			mirror_windows[i].decorate = !mirror_windows[i].decorate;
		}

		ui_window_end();
	}
}

int main(void) {
	DetourRestoreAfterWith();

	sk_settings_t settings = {};
	settings.app_name = "SKOverlayApp";
	settings.assets_folder = "Assets";
	settings.display_preference = display_mode_mixedreality;
	if (!sk_init(settings))
		return 1;

	// Hook the Varjo runtime to force overlay mode.
	// TODO: Use appropriate path.
	DetourDllAttach("C:\\Program Files\\Varjo\\varjo-openxr\\VarjoLib.dll", "varjo_WaitSync", hooked_varjo_WaitSync, original_varjo_WaitSync);

	// Disable skybox to ensure a transparent background.
	render_enable_skytex(false);

	// Resources for the scene.
	mirror_mesh = mesh_find(default_id_mesh_quad);
	ID3D11DeviceContext* context;
	render_get_device((void**)&mirror_device, (void**)&context);
	if (context) {
		context->Release();
	}

	DwmGetDxSharedSurface = (fn_GetDxSharedSurface)GetProcAddress(LoadLibrary("user32.dll"), "DwmGetDxSharedSurface");

	sk_run(step);

	return 0;
}
