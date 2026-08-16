-- include subprojects
includes("lib/commonlibf4")

-- FFMPEG is required
add_requires("ffmpeg")

-- set project constants
set_project("f4ffmpeg")
set_version("0.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

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
