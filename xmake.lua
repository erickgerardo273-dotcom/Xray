add_rules("mode.debug", "mode.release")

-- Preloader de LeviLauncher (trae pl/Mod.hpp, pl/memory/Hook.hpp, etc.)
package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("nlohmann_json v3.11.3")
add_requires("fmt")


function levi_target(name, src, icon, version, desc)
    target(name)
        set_kind("shared")
        set_languages("c++20")
        set_strip("all")
        add_files(src)
        add_packages("preloader", "nlohmann_json", "fmt")
        if is_plat("android") then
            add_cxflags("-fPIC", "-O2", "-ffunction-sections", "-fdata-sections", "-fexceptions", "-w")
            add_cxxflags("-fno-rtti")
            add_shflags("-Wl,--gc-sections", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384")
            add_links("android", "log", "EGL")
        end
        after_build(function (target)
            if not target:is_plat("android") then return end
            import("lib.detect.find_tool")
            local python = find_tool("python3") or find_tool("python")
            assert(python, "Se necesita Python 3")
            os.vrunv(python.program, {
                path.join(os.projectdir(), "package_levipack.py"),
                "--library", target:targetfile(),
                "--icon", path.join(os.projectdir(), icon),
                "--output", path.join(target:targetdir(), name .. ".levipack"),
                "--name", name, "--version", version, "--description", desc
            })
        end)
    target_end()
end

levi_target("XrayLevi", "xray.cpp", "icon_xray.png", "0.1.0",
    "X-Ray de minerales con boton flotante y menu. Hecho por erickzgcs.")
