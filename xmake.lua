--Set platform, cause some of us aren't on Windows.
set_plat("windows")
set_arch("x64")

-- f4ffmpeg uses REX::TTomlSetting / FTomlSettingStore. CommonLib-shared
-- keeps TOML support optional and disabled by default, so enable it before
-- loading CommonLibF4 so the option propagates into commonlib-shared.
set_config("commonlib_toml", true)

-- include subprojects
includes("lib/commonlibf4")

-- FFMPEG is required, and we want an explicitly vulkan build. We use a custom package for this. Feel free to go through the effort to make this some build flag.
add_repositories(
    "f4ffmpeg-repo xmake-packages",
    {rootdir = os.projectdir()}
)


-- Build FFmpeg with NVIDIA's NVDEC/CUDA decode path available. This remains
-- runtime-optional: systems without an NVIDIA driver/CUDA bridge simply skip
-- the backend and fall through to the remaining hardware APIs/software.
add_requires("ffmpeg", {
    configs = {
        ffmpeg = false,
        ffprobe = false,
        ffplay = false,
        nvdec = true,
        libzimg = false
    }
})

-- libplacebo is opportunistic. The normal preference order is:
--   1. statically linked libplacebo from the repository vcpkg overlay;
--   2. a compatible user-provided libplacebo DLL loaded at runtime;
--   3. libswscale.
--
-- f4ffmpeg itself never builds or bundles the shared DLL fallback.
option("libplacebo_mode")
    set_default("auto")
    set_showmenu(true)
    set_values(
        "auto",
        "static",
        "runtime",
        "off"
    )
    set_description(
        "libplacebo mode: auto/static/runtime/off"
    )
option_end()

local placebo_mode =
    get_config("libplacebo_mode") or
    "auto"

if placebo_mode == "static" then
    -- vcpkg owns libplacebo's Meson/tool/dependency environment. The repository
    -- overlay builds libplacebo itself with clang-cl while f4ffmpeg remains MSVC.
    add_requires(
        "vcpkg::libplacebo",
        {
            alias = "f4ffmpeg-libplacebo"
        }
    )
elseif placebo_mode == "auto" then
    add_requires(
        "vcpkg::libplacebo",
        {
            alias = "f4ffmpeg-libplacebo",
            optional = true
        }
    )
end

-- set project constants
set_project("f4ffmpeg")
set_version("0.0.1")
set_license("GPL-3.0")
set_languages("c11", "c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- override runtime count
add_defines("COMMONLIB_RUNTIMECOUNT=3")

-- define targets
target("f4ffmpeg")
    add_rules("commonlibf4.plugin", {
        name = "f4ffmpeg",
        author = "Continous",
        description = "This is a simple plugin intended to provide FFMPEG."
    })

    -- add src files
    add_files("src/**.cpp", "src/**.c")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- FFmpeg always provides demux/decode and the libswscale fallback.
    add_packages("ffmpeg")

    if placebo_mode == "off" then
        add_defines(
            "F4FFMPEG_HAS_LIBPLACEBO_STATIC=0",
            "F4FFMPEG_ALLOW_LIBPLACEBO_DLL=0"
        )
    elseif placebo_mode == "runtime" then
        -- Never link libplacebo in this mode. Future runtime dispatch may use
        -- only a compatible user-provided DLL.
        add_defines(
            "F4FFMPEG_HAS_LIBPLACEBO_STATIC=0",
            "F4FFMPEG_ALLOW_LIBPLACEBO_DLL=1"
        )
    elseif placebo_mode == "static" then
        -- add_requires() above is strict in this mode, so reaching the target
        -- means the static package was successfully resolved.
        add_packages("f4ffmpeg-libplacebo")
        add_defines(
            "PL_STATIC",
            "F4FFMPEG_HAS_LIBPLACEBO_STATIC=1",
            "F4FFMPEG_ALLOW_LIBPLACEBO_DLL=1"
        )
    elseif has_package("f4ffmpeg-libplacebo") then
        -- auto mode successfully resolved the preferred built-in package.
        add_packages("f4ffmpeg-libplacebo")
        add_defines(
            "PL_STATIC",
            "F4FFMPEG_HAS_LIBPLACEBO_STATIC=1",
            "F4FFMPEG_ALLOW_LIBPLACEBO_DLL=1"
        )
    else
        -- auto mode could not resolve the static package. Keep the plugin
        -- buildable and permit the optional user-supplied DLL path.
        add_defines(
            "F4FFMPEG_HAS_LIBPLACEBO_STATIC=0",
            "F4FFMPEG_ALLOW_LIBPLACEBO_DLL=1"
        )
    end
