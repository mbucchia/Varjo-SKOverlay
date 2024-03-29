#pragma once

#include <optional>
#include <string>

namespace Utils {

    template <typename TMethod>
    inline void DetourDllAttach(const wchar_t* dll, const char* target, TMethod hooked, TMethod& original) {
        if (original) {
            // Already hooked.
            return;
        }

        HMODULE handle;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, dll, &handle);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        original = (TMethod)GetProcAddress(handle, target);
        DetourAttach((PVOID*)&original, hooked);

        DetourTransactionCommit();
    }

    inline std::optional<std::wstring> RegGetString(HKEY hKey, const std::string& subKey, const std::string& value) {
        DWORD dataSize = 0;
        LONG retCode = ::RegGetValueW(hKey,
                                      std::wstring(subKey.cbegin(), subKey.cend()).c_str(),
                                      std::wstring(value.cbegin(), value.cend()).c_str(),
                                      RRF_SUBKEY_WOW6464KEY | RRF_RT_REG_SZ,
                                      nullptr,
                                      nullptr,
                                      &dataSize);
        if (retCode != ERROR_SUCCESS || !dataSize) {
            return {};
        }

        std::wstring data(dataSize / sizeof(wchar_t), 0);
        retCode = ::RegGetValueW(hKey,
                                 std::wstring(subKey.cbegin(), subKey.cend()).c_str(),
                                 std::wstring(value.cbegin(), value.cend()).c_str(),
                                 RRF_SUBKEY_WOW6464KEY | RRF_RT_REG_SZ,
                                 nullptr,
                                 data.data(),
                                 &dataSize);
        if (retCode != ERROR_SUCCESS) {
            return {};
        }

        return data.substr(0, dataSize / sizeof(wchar_t) - 1);
    }

} // namespace Utils
