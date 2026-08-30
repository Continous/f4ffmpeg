# Static Windows libplacebo port for f4ffmpeg.
#
# Packaging strategy follows the proven Windows/vcpkg pattern used by
# LichtFeld Studio:
#   - vcpkg owns build tools and third-party dependencies
#   - clang-cl is used only for libplacebo itself
#   - upstream Meson remains responsible for building libplacebo
#
# f4ffmpeg differs in one place: it needs the D3D11 backend and wants the
# embedded route fully static, so libplacebo's SPIRV-Cross pkg-config lookup
# is redirected from the shared C API name to vcpkg's static C API name.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO haasn/libplacebo
    REF v7.360.1
    SHA512 209B1713CFF34F06149AF16FB3EA52E3662A566EF5DF6B29811AD295AA8CB6388F827A93FC8E0EED1A72F35B3B3AAE835520C933079E706A51D11136A8128799
    HEAD_REF master
)

# GitHub source archives do not contain libplacebo's nested Jinja/MarkupSafe
# sources. Vendor the same pinned sources used by the known-working Windows
# vcpkg port rather than teaching Xmake how to assemble them.
vcpkg_from_github(
    OUT_SOURCE_PATH JINJA_SOURCE
    REPO pallets/jinja
    REF 15206881c006c79667fe5154fe80c01c65410679
    SHA512 E1082222A4660E60F05E970E7C5B6F2FAB377BA01C273BCB6FE0EAD457EA5D4764C1D95FB3264B6BC371E122D574517AC35B6AE3858B50BC4918ACD08A3F75DE
    HEAD_REF main
)

vcpkg_from_github(
    OUT_SOURCE_PATH MARKUPSAFE_SOURCE
    REPO pallets/markupsafe
    REF 297fc8e356e6836a62087949245d09a28e9f1b13
    SHA512 8E16146B42DE9F0939B706C1652D4C5FE8E67E1F7E0C5A0E37D698D9AB10DCADF3E26B12E4BE2B37209C33703996351B02C54AF7CEB2D9EAF24AEDE7CECDF648
    HEAD_REF main
)

file(COPY "${JINJA_SOURCE}/src/"
     DESTINATION "${SOURCE_PATH}/3rdparty/jinja/src")
file(COPY "${MARKUPSAFE_SOURCE}/src/"
     DESTINATION "${SOURCE_PATH}/3rdparty/markupsafe/src")

# vcpkg's SPIRV-Cross port is deliberately static-only and installs the C API
# as `spirv-cross-c`. libplacebo 7.360.1 asks for the shared pkg-config name
# when D3D11 is enabled. Keep the version/required logic intact and change
# only the dependency identity.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/d3d11/meson.build"
    "spirv-cross-c-shared"
    "spirv-cross-c"
)

# libplacebo is happier with clang-cl on Windows, while f4ffmpeg itself can
# remain an ordinary MSVC build. Locate the VS/LLVM compiler and override
# only Meson's C/C++ compiler for this port.
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    find_program(_f4ffmpeg_clang_cl NAMES clang-cl clang-cl.exe)

    if(NOT _f4ffmpeg_clang_cl AND DEFINED ENV{VCINSTALLDIR})
        file(TO_CMAKE_PATH "$ENV{VCINSTALLDIR}" _vcinstall)
        set(_candidate "${_vcinstall}/Tools/Llvm/x64/bin/clang-cl.exe")
        if(EXISTS "${_candidate}")
            set(_f4ffmpeg_clang_cl "${_candidate}")
        endif()
    endif()

    if(NOT _f4ffmpeg_clang_cl AND DEFINED ENV{VSINSTALLDIR})
        file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _vsinstall)
        set(_candidate "${_vsinstall}/VC/Tools/Llvm/x64/bin/clang-cl.exe")
        if(EXISTS "${_candidate}")
            set(_f4ffmpeg_clang_cl "${_candidate}")
        endif()
    endif()

    if(NOT _f4ffmpeg_clang_cl)
        message(FATAL_ERROR
            "libplacebo requires clang-cl on Windows. Install the Visual Studio "
            "'C++ Clang Compiler for Windows' component or put clang-cl on PATH.")
    endif()

    file(TO_CMAKE_PATH "${_f4ffmpeg_clang_cl}" _clang_cl_meson)
    set(_clang_native_file
        "${CURRENT_BUILDTREES_DIR}/clang-cl-${TARGET_TRIPLET}.ini")

    file(WRITE "${_clang_native_file}"
        "[binaries]\n"
        "c = ['${_clang_cl_meson}']\n"
        "cpp = ['${_clang_cl_meson}']\n")

    set(VCPKG_MESON_NATIVE_FILE_DEBUG "${_clang_native_file}")
    set(VCPKG_MESON_NATIVE_FILE_RELEASE "${_clang_native_file}")
endif()

# Use vcpkg's own Meson/pkgconf plumbing instead of reproducing it in Xmake.
include("${CURRENT_HOST_INSTALLED_DIR}/share/vcpkg-tool-meson/vcpkg-port-config.cmake")

set(LIBPLACEBO_MESON_OPTIONS
    -Ddefault_library=static
    -Dd3d11=enabled
    -Dshaderc=enabled
    -Dglslang=disabled
    -Dvulkan=disabled
    -Dopengl=disabled
    -Ddovi=disabled
    -Dlibdovi=disabled
    -Dlcms=disabled
    -Dxxhash=disabled
    -Dunwind=disabled
    -Ddemos=false
    -Dtests=false
    -Dbench=false
    -Dfuzz=false
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${LIBPLACEBO_MESON_OPTIONS}
)

vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
