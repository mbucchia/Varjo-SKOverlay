#pragma once

template <typename TMethod>
inline void DetourDllAttach(const char* dll, const char* target, TMethod hooked, TMethod& original) {
    if (original) {
        // Already hooked.
        return;
    }

    HMODULE handle;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, dll, &handle);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    original = (TMethod)GetProcAddress(handle, target);
    DetourAttach((PVOID*)&original, hooked);

    DetourTransactionCommit();
}
