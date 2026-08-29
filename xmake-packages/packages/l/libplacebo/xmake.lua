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
        "pkgconf",
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

        -- Upstream libplacebo 7.360.1's D3D11 Meson file asks specifically for
        -- `spirv-cross-c-shared`. That is useful for normal distro builds, but
        -- f4ffmpeg's built-in path must not acquire a runtime SPIRV-Cross DLL.
        --
        -- Replace only that dependency identity. The SPIRV-Cross C API is the
        -- same; our dependency above deliberately installs its static C target.
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

        local before =
            io.readfile(d3d11_meson)

        assert(
            before:find(
                "spirv-cross-c-shared",
                1,
                true
            ),
            "libplacebo D3D11 Meson definition no longer contains " ..
            "'spirv-cross-c-shared'; review the package patch for this version"
        )

        io.replace(
            d3d11_meson,
            "spirv-cross-c-shared",
            "spirv-cross-c",
            {plain = true}
        )

        local after =
            io.readfile(d3d11_meson)

        assert(
            not after:find(
                "spirv-cross-c-shared",
                1,
                true
            ),
            "failed to patch libplacebo D3D11 SPIRV-Cross dependency"
        )

        print(
            "Patched libplacebo D3D11 dependency: " ..
            "spirv-cross-c-shared -> spirv-cross-c"
        )

        local configs = {
            "-Ddefault_library=static",
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
