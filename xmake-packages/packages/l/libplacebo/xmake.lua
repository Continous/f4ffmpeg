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

    -- Build tools only.
    add_deps(
        "meson",
        "ninja",
        {kind = "binary", private = true}
    )

    -- f4ffmpeg's built-in libplacebo path is intentionally fully static.
    add_deps(
        "shaderc",
        {configs = {shared = false}}
    )

    add_deps(
        "spirv-cross",
        {configs = {shared = false}}
    )

    on_load("windows", function (package)
        -- This package deliberately exposes only a static libplacebo build.
        package:add("defines", "PL_STATIC")
        package:add("links", "libplacebo")

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
        local function dump_dependency(name, expected_pc)
            local dep = package:dep(name)

            assert(
                dep,
                "libplacebo dependency '" ..
                    name ..
                    "' was not resolved"
            )

            print(
                string.format(
                    "libplacebo dependency %s: %s",
                    name,
                    dep:installdir()
                )
            )

            local pcfiles =
                os.files(
                    path.join(
                        dep:installdir(),
                        "**.pc"
                    )
                )

            if #pcfiles == 0 then
                print(
                    "  pkg-config metadata: <none found>"
                )
            else
                print("  pkg-config metadata:")
                for _, pcfile in ipairs(pcfiles) do
                    print(
                        "    " ..
                        path.relative(
                            pcfile,
                            dep:installdir()
                        )
                    )
                end
            end

            if expected_pc then
                local found = false

                for _, pcfile in ipairs(pcfiles) do
                    if path.filename(pcfile) == expected_pc then
                        found = true
                        break
                    end
                end

                if not found then
                    print(
                        "  NOTE: expected " ..
                        expected_pc ..
                        " was not found. If Meson dependency discovery fails, " ..
                        "inspect this dependency first."
                    )
                end
            end
        end

        dump_dependency(
            "shaderc",
            "shaderc.pc"
        )

        dump_dependency(
            "spirv-cross",
            "spirv-cross-c.pc"
        )

        -- Our Windows/MSVC package already knows exactly where its static
        -- dependencies live. Avoid libplacebo's pkg-config-only discovery
        -- path and point Meson directly at the static libraries.
        --
        -- This also prevents the D3D11 backend from requesting the upstream
        -- `spirv-cross-c-shared` dependency and accidentally introducing a
        -- runtime SPIRV-Cross DLL.
        local shaderc_dep =
            package:dep("shaderc")
        local spirv_cross_dep =
            package:dep("spirv-cross")

        assert(shaderc_dep, "shaderc dependency was not resolved")
        assert(spirv_cross_dep, "spirv-cross dependency was not resolved")

        -- Meson source strings must not receive raw Windows backslashes.
        -- Sequences such as \a and \f are interpreted as escapes, which
        -- corrupts paths like D:\a\f4ffmpeg before find_library() sees them.
        local shaderc_libdir =
            shaderc_dep:installdir("lib"):gsub("\\", "/")
        local shaderc_incdir =
            shaderc_dep:installdir("include"):gsub("\\", "/")

        local spirv_cross_libdir =
            spirv_cross_dep:installdir("lib"):gsub("\\", "/")
        local spirv_cross_incdir =
            spirv_cross_dep:installdir("include"):gsub("\\", "/")

        local glsl_meson =
            path.join(
                "src",
                "glsl",
                "meson.build"
            )

        assert(
            os.isfile(glsl_meson),
            "libplacebo GLSL Meson definition was not found"
        )

        local glsl_before =
            io.readfile(glsl_meson)

        local shaderc_lookup =
            "shaderc = dependency('shaderc', version: '>=2019.1', required: get_option('shaderc'))"

        assert(
            glsl_before:find(
                shaderc_lookup,
                1,
                true
            ),
            "libplacebo shaderc lookup no longer matches 7.360.1; " ..
            "review the static shaderc patch"
        )

        local shaderc_replacement =
            "shaderc_lib = cc.find_library('shaderc_combined', " ..
            "required: get_option('shaderc'), " ..
            "dirs: '" .. shaderc_libdir .. "')\n" ..
            "shaderc = declare_dependency(\n" ..
            "  dependencies: shaderc_lib,\n" ..
            "  include_directories: ['" .. shaderc_incdir .. "'],\n" ..
            "  version: '2024.1',\n" ..
            ")"

        io.replace(
            glsl_meson,
            shaderc_lookup,
            shaderc_replacement,
            {plain = true}
        )

        local glsl_after =
            io.readfile(glsl_meson)

        assert(
            glsl_after:find(
                "shaderc = declare_dependency(",
                1,
                true
            ),
            "failed to patch libplacebo shaderc discovery"
        )

        local d3d11_meson =
            path.join(
                "src",
                "d3d11",
                "meson.build"
            )

        assert(
            os.isfile(d3d11_meson),
            "libplacebo D3D11 Meson definition was not found"
        )

        local d3d_before =
            io.readfile(d3d11_meson)

        assert(
            d3d_before:find(
                "dependency('spirv-cross-c-shared'",
                1,
                true
            ),
            "libplacebo D3D11 SPIRV-Cross lookup no longer matches 7.360.1; " ..
            "review the static SPIRV-Cross patch"
        )

        -- Replace the complete upstream dependency() expression. Doing only a
        -- prefix replacement leaves dependency-only keywords such as
        -- `version:` attached to cc.find_library(), which Meson rejects.
        local spirv_pattern =
            "spirv_cross%s*=%s*" ..
            "dependency%('spirv%-cross%-c%-shared',%s*" ..
            "version:%s*'[^']+',%s*" ..
            "required:%s*get_option%('d3d11'%)%s*" ..
            "%)"

        local spirv_replacement =
            "spirv_cross_lib = cc.find_library('spirv-cross-c', " ..
            "required: get_option('d3d11'), " ..
            "dirs: '" .. spirv_cross_libdir .. "')\n" ..
            "spirv_cross = declare_dependency(\n" ..
            "  dependencies: spirv_cross_lib,\n" ..
            "  include_directories: ['" .. spirv_cross_incdir .. "'],\n" ..
            "  version: '0.57.0',\n" ..
            ")"

        local d3d_after, spirv_replacements =
            d3d_before:gsub(
                spirv_pattern,
                spirv_replacement,
                1
            )

        assert(
            spirv_replacements == 1,
            "failed to replace the complete libplacebo SPIRV-Cross " ..
            "dependency expression"
        )

        io.writefile(
            d3d11_meson,
            d3d_after
        )

        assert(
            d3d_after:find(
                "spirv_cross = declare_dependency(",
                1,
                true
            ),
            "failed to patch libplacebo SPIRV-Cross discovery"
        )

        assert(
            not d3d_after:find(
                "spirv-cross-c-shared",
                1,
                true
            ),
            "shared SPIRV-Cross dependency remained after patch"
        )

        print(
            "Patched libplacebo dependency discovery for static MSVC: " ..
            "shaderc_combined.lib + spirv-cross-c.lib + headers (no pkg-config)"
        )

        -- MSVC exposes C11 atomics only behind /experimental:c11atomics.
        -- Pass the flag through Meson's built-in c_args option so it applies
        -- to both configure-time feature probes and the actual C sources.
        local configs = {
            "-Ddefault_library=static",
            "-Dc_args=/experimental:c11atomics",
            "-Dauto_features=disabled",

            -- Graphics API.
            "-Dd3d11=enabled",
            "-Dvulkan=disabled",
            "-Dopengl=disabled",

            -- Shader compiler.
            "-Dshaderc=enabled",
            "-Dglslang=disabled",

            -- Optional integrations not needed by f4ffmpeg yet.
            "-Dlcms=disabled",
            "-Ddovi=disabled",
            "-Dlibdovi=disabled",
            "-Dxxhash=disabled",
            "-Dunwind=disabled",

            -- No unrelated executables/tests.
            "-Ddemos=false",
            "-Dtests=false",
            "-Dbench=false",
            "-Dfuzz=false",
            "-Ddebug-abort=false"
        }

        print(
            "Configuring static libplacebo 7.360.1 " ..
            "for Windows/x64 (D3D11 only)"
        )

        import("package.tools.meson").install(
            package,
            configs,
            {
                packagedeps = {
                    "shaderc",
                    "spirv-cross"
                }
            }
        )
    end)

    on_test(function (package)
        assert(
            package:has_cfuncs(
                "pl_renderer_create",
                {
                    includes =
                        "libplacebo/renderer.h"
                }
            )
        )

        assert(
            package:has_cfuncs(
                "pl_d3d11_create",
                {
                    includes =
                        "libplacebo/d3d11.h"
                }
            )
        )
    end)
