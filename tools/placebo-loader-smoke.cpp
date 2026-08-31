#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "placeboBackendApi.h"

#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"usage: placebo-loader-smoke <f4ffmpeg_placebo.dll>\n";
        return 2;
    }

    HMODULE module = LoadLibraryExW(
        argv[1],
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    );

    if (module == nullptr)
    {
        std::wcerr << L"LoadLibraryExW failed: " << GetLastError() << L"\n";
        return 3;
    }

    const auto abi =
        reinterpret_cast<f4ffmpeg_placebo_abi_fn>(
            GetProcAddress(
                module,
                "f4ffmpeg_placebo_backend_abi"
            )
        );

    const auto convert =
        reinterpret_cast<f4ffmpeg_placebo_convert_fn>(
            GetProcAddress(
                module,
                "f4ffmpeg_placebo_convert"
            )
        );

    const bool valid =
        abi != nullptr &&
        convert != nullptr &&
        abi() == F4FFMPEG_PLACEBO_BACKEND_ABI;

    FreeLibrary(module);

    if (!valid)
    {
        std::cerr << "backend ABI/symbol validation failed\n";
        return 4;
    }

    std::cout << "f4ffmpeg_placebo backend load/ABI smoke passed\n";
    return 0;
}
