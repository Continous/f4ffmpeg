# Static Windows libplacebo port for f4ffmpeg.
#
# Packaging strategy follows the proven Windows/vcpkg pattern used by
# LichtFeld Studio:
#   - vcpkg owns build tools and third-party dependencies
#   - clang-cl is used only for libplacebo itself
#   - upstream Meson remains responsible for building libplacebo
#
# f4ffmpeg's optional presentation companion uses libplacebo only through its
# D3D11 backend. FFmpeg's Vulkan Video decoder is independent and does not need
# libplacebo's Vulkan backend, so keep the companion D3D11-only. D3D11 needs
# vcpkg's static SPIRV-Cross C API. Shader compilation deliberately uses
# libplacebo's direct glslang backend, avoiding the shaderc 2026.2 compiler-crash
# regression implicated by the first-render failure in Fallout.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# f4ffmpeg only ships the release/releasedbg companion. vcpkg's Debug glslang
# archives use the conventional `d` suffix (for example SPIRVd.lib), while
# libplacebo 7.360.1's Meson glslang probe asks for the unsuffixed library
# names even during a Debug configure. Avoid configuring an unused Debug
# libplacebo variant that therefore cannot discover vcpkg's Debug glslang.
set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO haasn/libplacebo
    REF v7.360.1
    SHA512 209B1713CFF34F06149AF16FB3EA52E3662A566EF5DF6B29811AD295AA8CB6388F827A93FC8E0EED1A72F35B3B3AAE835520C933079E706A51D11136A8128799
    HEAD_REF master
)

# vcpkg's SPIRV-Cross port is deliberately static-only and installs the C API
# as `spirv-cross-c`. libplacebo 7.360.1 asks for the shared pkg-config name
# when D3D11 is enabled. Keep the version/required logic intact and change
# only the dependency identity.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/d3d11/meson.build"
    "spirv-cross-c-shared"
    "spirv-cross-c"
)

# Meson's compiler.find_library(static: true) does not reliably honor vcpkg's
# linker search path. libplacebo 7.360.x uses find_library() directly for the
# glslang/SPIR-V closure, which can therefore report `SPIRV` missing even
# though vcpkg installed it successfully. Explicitly add the active target
# triplet's library directory to every glslang probe. This mirrors the
# established downstream workaround used by other package systems, but keeps
# the path scoped to this vcpkg port.
file(TO_CMAKE_PATH "${CURRENT_INSTALLED_DIR}/lib" _f4ffmpeg_glslang_libdir)

vcpkg_replace_string(
    "${SOURCE_PATH}/src/glsl/meson.build"
    "  glslang_deps = ["
    "  glslang_vcpkg_lib_dirs = ['${_f4ffmpeg_glslang_libdir}']\n  glslang_deps = ["
)

vcpkg_replace_string(
    "${SOURCE_PATH}/src/glsl/meson.build"
    "cxx.find_library('glslang-default-resource-limits', required: false)"
    "cxx.find_library('glslang-default-resource-limits', required: false, dirs: glslang_vcpkg_lib_dirs)"
)

vcpkg_replace_string(
    "${SOURCE_PATH}/src/glsl/meson.build"
    "cxx.find_library('glslang', required: required, static: static)"
    "cxx.find_library('glslang', required: required, static: static, dirs: glslang_vcpkg_lib_dirs + vulkan_lib_dirs)"
)

vcpkg_replace_string(
    "${SOURCE_PATH}/src/glsl/meson.build"
    "dirs: vulkan_lib_dirs)"
    "dirs: glslang_vcpkg_lib_dirs + vulkan_lib_dirs)"
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
    -Dshaderc=disabled
    -Dglslang=enabled
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
