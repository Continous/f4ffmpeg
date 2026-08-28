--Set platform, cause some of us aren't on Windows.
set_plat("windows")
set_arch("x64")

-- Commonlib toml library.
set_config("commonlib_toml", true)

-- include subprojects
includes("lib/commonlibf4")

-- FFMPEG is required, and we want an explicitly vulkan build. We use a custom package for this. Feel free to go through the effort to make this some build flag.
add_repositories(
    "f4ffmpeg-repo xmake-packages",
    {rootdir = os.projectdir()}
)

-- libplacebo is not currently available from xmake.
package("libplacebo")
    set_homepage("https://libplacebo.org")
    set_description("Reusable library for GPU-accelerated image/video processing")
    set_license("LGPL-2.1-or-later")

    add_urls(
        "https://github.com/haasn/libplacebo.git",
        {submodules = true}
    )

    add_versions(
        "7.360.1",
        "cee9b076f2c63104ccfd497fa79c39a867293ec4"
    )

    add_deps("meson", "ninja", "pkgconf")
    add_deps("shaderc")
    add_deps("spirv-cross", {configs = {shared = true}})

    on_load("windows", function (package)
        package:add("defines", "PL_STATIC")
        package:add(
            "syslinks",
            "d3d11",
            "dxgi",
            "dxguid",
            "d3dcompiler",
            "shlwapi",
            "version"
        )
    end)

    on_install("windows|x64", function (package)
        local configs = {
            "-Ddefault_library=static",
            "-Dd3d11=enabled",
            "-Dvulkan=disabled",
            "-Dopengl=disabled",
            "-Dshaderc=enabled",
            "-Dglslang=disabled",
            "-Dlcms=disabled",
            "-Dlibdovi=disabled",
            "-Dxxhash=disabled",
            "-Dunwind=disabled",
            "-Ddemos=false",
            "-Dtests=false",
            "-Dbench=false",
            "-Dfuzz=false"
        }

        import("package.tools.meson").install(
            package,
            configs
        )
    end)

    on_test(function (package)
        assert(
            package:has_cfuncs(
                "pl_renderer_create",
                {includes = "libplacebo/renderer.h"}
            )
        )
    end)
package_end()

-- Build FFmpeg with nvdec.
add_requires("ffmpeg", {
    configs = {
        ffmpeg = false,
        ffprobe = false,
        ffplay = false,
        nvdec = true,
        libzimg = false
    }
})

-- libplacebo
add_requires("libplacebo 7.360.1")

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
    -- FFmpeg demuxes/decodes. libplacebo performs presentation conversion.
    add_packages("ffmpeg", "libplacebo")
