--Set platform, cause some of us aren't on Windows.
set_plat("windows")
set_arch("x64")

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
        nvdec = true
    }
})

-- set project constants
set_project("f4ffmpeg")
set_version("0.0.1")
set_license("GPL-3.0")
set_languages("c++23")
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
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    -- Use FFMPEG
    add_packages("ffmpeg")
