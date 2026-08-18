#include "graphics.h"
#include "pch.h"

#include <RE/B/BSGraphics.h>

namespace f4ffmpeg
{
    namespace
    {
        REX::W32::ID3D11Device* d3d11Device = nullptr;
        REX::W32::ID3D11DeviceContext* d3d11DeviceContext = nullptr;

        bool graphicsInitialized = false;
    }

    bool initializeGraphics()
    {
        auto* rendererData = RE::BSGraphics::GetRendererData();

        if (rendererData == nullptr)
        {
            return false;
        }

        if (!rendererData->initialized)
        {
            return false;
        }

        if (rendererData->device == nullptr)
        {
            return false;
        }

        if (rendererData->context == nullptr)
        {
            return false;
        }

        d3d11Device = rendererData->device;
        d3d11DeviceContext = rendererData->context;

        graphicsInitialized = true;
        const auto deviceFlags =
            falloutDevice->GetCreationFlags();

        constexpr std::uint32_t videoSupportFlag =
            0x800;

        REX::INFO(
            "Fallout D3D11 creation flags: 0x{:X}",
            deviceFlags
        );

        REX::INFO(
            "Fallout D3D11 VIDEO_SUPPORT flag: {}",
            (deviceFlags & videoSupportFlag) != 0
        );
        return true;
    }

    bool isGraphicsInitialized()
    {
        return graphicsInitialized;
    }

    REX::W32::ID3D11Device* getD3D11Device()
    {
        return d3d11Device;
    }

    REX::W32::ID3D11DeviceContext* getD3D11DeviceContext()
    {
        return d3d11DeviceContext;
    }
}
