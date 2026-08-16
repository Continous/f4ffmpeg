#pragma once

namespace REX::W32
{
    struct ID3D11Device;
    struct ID3D11DeviceContext;
}

namespace f4ffmpeg
{
    bool initializeGraphics();
    bool isGraphicsInitialized();

    REX::W32::ID3D11Device* getD3D11Device();
    REX::W32::ID3D11DeviceContext* getD3D11DeviceContext();
}
