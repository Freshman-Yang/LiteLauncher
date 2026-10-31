cmake_minimum_required(VERSION 3.21)

project(LiteLauncher
    VERSION 1.0.0
    DESCRIPTION "Lightweight Windows Launcher"
    LANGUAGES CXX RC
)

if(NOT WIN32)
    message(FATAL_ERROR "LiteLauncher only supports Windows platforms.")
endif()

add_executable(LiteLauncher WIN32)

target_sources(LiteLauncher
    PRIVATE
        LiteLauncher/pch.h
        LiteLauncher/main.cpp
        LiteLauncher/Assets/resource.h
        LiteLauncher/Assets/LiteLauncher.rc
        LiteLauncher/Assets/app.manifest
        LiteLauncher/Core/ConfigManager.h
        LiteLauncher/Core/ConfigManager.cpp
        LiteLauncher/Core/I18n.h
        LiteLauncher/Core/SearchManager.h
        LiteLauncher/Core/SearchManager.cpp
        LiteLauncher/Providers/Win32Provider.h
        LiteLauncher/Providers/Win32Provider.cpp
        LiteLauncher/Providers/UwpProvider.h
        LiteLauncher/Providers/UwpProvider.cpp
        LiteLauncher/Providers/SettingsProvider.h
        LiteLauncher/Utils/PinyinMatcher.h
        LiteLauncher/Utils/PinyinMatcher.cpp
        LiteLauncher/Utils/ProcessHelper.h
        LiteLauncher/Utils/ProcessHelper.cpp
        LiteLauncher/UI/IconManager.h
        LiteLauncher/UI/IconManager.cpp
        LiteLauncher/UI/LauncherWindow.h
        LiteLauncher/UI/LauncherWindow.cpp
        LiteLauncher/UI/SettingsWindow.h
        LiteLauncher/UI/SettingsWindow.cpp
)

target_compile_features(LiteLauncher PRIVATE cxx_std_20)

set_target_properties(LiteLauncher PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

target_include_directories(LiteLauncher
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/LiteLauncher"
)

target_precompile_headers(LiteLauncher
    PRIVATE
        LiteLauncher/pch.h
)

target_compile_definitions(LiteLauncher
    PRIVATE
        UNICODE
        _UNICODE
        WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
)

if(MSVC)
    target_compile_options(LiteLauncher
        PRIVATE
            /W4
            /utf-8
            /permissive-
            $<$<CONFIG:Release>:/Oi /Gy /Gw>
    )

    target_link_options(LiteLauncher
        PRIVATE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /DEBUG:NONE /Brepro>
    )
endif()

target_link_libraries(LiteLauncher
    PRIVATE
        windowsapp
        d2d1
        dwrite
        windowscodecs
        dwmapi
        shcore
        comctl32
        gdi32
        imm32
        user32
        shell32
        ole32
        advapi32
        uxtheme
)

include(GNUInstallDirs)
install(TARGETS LiteLauncher
    RUNTIME DESTINATION .
)

