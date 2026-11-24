cmake_minimum_required(VERSION 3.20)

project(DagorAssetsExtractor LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DagorAssetsExtractor currently supports Windows builds only.")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(APP_DIR "${PROJECT_ROOT}/DagorAssetsExtractor")
set(APP_SRC_DIR "${APP_DIR}/src")
set(THIRD_PARTY_DIR "${PROJECT_ROOT}/third_party")

file(GLOB_RECURSE DAGOR_CORE_SOURCES CONFIGURE_DEPENDS
    "${APP_SRC_DIR}/*.cpp"
    "${APP_SRC_DIR}/*.h"
)

list(REMOVE_ITEM DAGOR_CORE_SOURCES
    "${APP_SRC_DIR}/main.cpp"
    "${APP_SRC_DIR}/UI/UI.cpp"
    "${APP_SRC_DIR}/UI/UI.h"
    "${APP_SRC_DIR}/UI/UiAssets.cpp"
    "${APP_SRC_DIR}/UI/UiAssets.h"
    "${APP_SRC_DIR}/UI/ModelRenderer.cpp"
    "${APP_SRC_DIR}/UI/ModelRenderer.h"
)

set(DAGOR_UI_SOURCES
    "${APP_SRC_DIR}/UI/UI.cpp"
    "${APP_SRC_DIR}/UI/UI.h"
    "${APP_SRC_DIR}/UI/UiAssets.cpp"
    "${APP_SRC_DIR}/UI/UiAssets.h"
    "${APP_SRC_DIR}/UI/ModelRenderer.cpp"
    "${APP_SRC_DIR}/UI/ModelRenderer.h"
)

set(DAGOR_IMGUI_SOURCES
    "${THIRD_PARTY_DIR}/imgui/imgui.cpp"
    "${THIRD_PARTY_DIR}/imgui/imgui_draw.cpp"
    "${THIRD_PARTY_DIR}/imgui/imgui_tables.cpp"
    "${THIRD_PARTY_DIR}/imgui/imgui_widgets.cpp"
    "${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_dx11.cpp"
    "${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_dx11.h"
    "${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_win32.cpp"
    "${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_win32.h"
    "${THIRD_PARTY_DIR}/imgui/imgui.h"
    "${THIRD_PARTY_DIR}/imgui/imgui_internal.h"
    "${THIRD_PARTY_DIR}/imgui/imconfig.h"
)

set(DAGOR_COMPRESSION_SOURCES
    "${THIRD_PARTY_DIR}/adler32.c"
    "${THIRD_PARTY_DIR}/crc32.c"
    "${THIRD_PARTY_DIR}/LzmaDec.c"
    "${THIRD_PARTY_DIR}/tinflate.c"
    "${THIRD_PARTY_DIR}/tinfzlib.c"
    "${THIRD_PARTY_DIR}/zstddeclib.c"
)

set(DAGOR_DIRECTXTEX_SOURCES
    "${THIRD_PARTY_DIR}/DirectXTex/BC.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/BC4BC5.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/BC6HBC7.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexCompress.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexConvert.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexDDS.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexFlipRotate.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexHDR.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexImage.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexMipmaps.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexMisc.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexNormalMaps.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexPMAlpha.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexResize.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexTGA.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexUtil.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTexWIC.cpp"
    "${THIRD_PARTY_DIR}/DirectXTex/DirectXTex.h"
)

function(dagor_apply_common_settings target_name)
    target_compile_definitions(${target_name}
        PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    target_compile_options(${target_name}
        PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /MP>
            $<$<C_COMPILER_ID:MSVC>:/W4 /MP>
    )

    set_target_properties(${target_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/windows/x86_64"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/windows/x86_64"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/windows/x86_64"
    )
endfunction()

add_library(dagor_vendor_compression STATIC ${DAGOR_COMPRESSION_SOURCES})
target_include_directories(dagor_vendor_compression PUBLIC "${THIRD_PARTY_DIR}")
dagor_apply_common_settings(dagor_vendor_compression)
set_target_properties(dagor_vendor_compression PROPERTIES FOLDER "ThirdParty")

add_library(dagor_directxtex STATIC ${DAGOR_DIRECTXTEX_SOURCES})
target_include_directories(dagor_directxtex PUBLIC "${THIRD_PARTY_DIR}/DirectXTex")
target_link_libraries(dagor_directxtex PUBLIC d3d11 dxgi windowscodecs)
dagor_apply_common_settings(dagor_directxtex)
set_target_properties(dagor_directxtex PROPERTIES FOLDER "ThirdParty")

add_library(dagor_imgui STATIC ${DAGOR_IMGUI_SOURCES})
target_include_directories(dagor_imgui
    PUBLIC
        "${THIRD_PARTY_DIR}/imgui"
        "${THIRD_PARTY_DIR}/imgui/backends"
)
target_link_libraries(dagor_imgui PUBLIC d3d11 dxgi)
dagor_apply_common_settings(dagor_imgui)
set_target_properties(dagor_imgui PROPERTIES FOLDER "ThirdParty")

add_library(dagor_core STATIC ${DAGOR_CORE_SOURCES})
target_include_directories(dagor_core
    PUBLIC
        "${APP_SRC_DIR}"
        "${THIRD_PARTY_DIR}"
        "${THIRD_PARTY_DIR}/DirectXTex"
)
target_link_libraries(dagor_core
    PUBLIC
        dagor_directxtex
        dagor_vendor_compression
        ole32
        shell32
        windowscodecs
)
dagor_apply_common_settings(dagor_core)
set_target_properties(dagor_core PROPERTIES FOLDER "Dagor")

add_executable(DagorAssetsExtractor WIN32
    "${APP_SRC_DIR}/main.cpp"
    ${DAGOR_UI_SOURCES}
)
target_include_directories(DagorAssetsExtractor PRIVATE "${APP_SRC_DIR}")
target_link_libraries(DagorAssetsExtractor
    PRIVATE
        dagor_core
        dagor_imgui
        d3d11
        d3dcompiler
        dxgi
        dwmapi
        gdi32
        imm32
        ole32
        shell32
        user32
        windowscodecs
)
dagor_apply_common_settings(DagorAssetsExtractor)
set_target_properties(DagorAssetsExtractor PROPERTIES
    FOLDER "Dagor"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:DagorAssetsExtractor>"
)

add_executable(DagorAssetsExtractorValidationTests
    "${PROJECT_ROOT}/tests/ValidationMain.cpp"
)
target_include_directories(DagorAssetsExtractorValidationTests PRIVATE "${APP_SRC_DIR}")
target_link_libraries(DagorAssetsExtractorValidationTests PRIVATE dagor_core)
dagor_apply_common_settings(DagorAssetsExtractorValidationTests)
set_target_properties(DagorAssetsExtractorValidationTests PROPERTIES
    FOLDER "Dagor"
)

add_custom_command(TARGET DagorAssetsExtractor POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DagorAssetsExtractor>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${APP_DIR}/assets/ui"
        "$<TARGET_FILE_DIR:DagorAssetsExtractor>/assets/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${THIRD_PARTY_DIR}/oo2core_6_win64.dll"
        "$<TARGET_FILE_DIR:DagorAssetsExtractor>/oo2core_6_win64.dll"
)

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT DagorAssetsExtractor)
