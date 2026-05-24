workspace "DagorAssetsExtractor"
   architecture "x64"
   configurations { "Debug", "Release" }
   startproject "DagorAssetsExtractorApp"

   systemversion "latest"
   cppdialect "C++20"
   staticruntime "Off"

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"

   -- Project 1: DirectXTex
   project "DirectXTex"
      kind "StaticLib"
      language "C++"
      location "third_party/DirectXTex"
      targetdir "build/bin/%{cfg.buildcfg}"
      objdir "build/obj/%{cfg.buildcfg}/DirectXTex"

      files {
         "third_party/DirectXTex/BC.h",
         "third_party/DirectXTex/BC.cpp",
         "third_party/DirectXTex/BC4BC5.cpp",
         "third_party/DirectXTex/BC6HBC7.cpp",
         "third_party/DirectXTex/BCDirectCompute.h",
         "third_party/DirectXTex/BCDirectCompute.cpp",
         "third_party/DirectXTex/DDS.h",
         "third_party/DirectXTex/filters.h",
         "third_party/DirectXTex/scoped.h",
         "third_party/DirectXTex/DirectXTex.h",
         "third_party/DirectXTex/DirectXTexP.h",
         "third_party/DirectXTex/DirectXTex.inl",
         "third_party/DirectXTex/DirectXTexCompress.cpp",
         "third_party/DirectXTex/DirectXTexCompressGPU.cpp",
         "third_party/DirectXTex/DirectXTexConvert.cpp",
         "third_party/DirectXTex/DirectXTexD3D11.cpp",
         "third_party/DirectXTex/DirectXTexDDS.cpp",
         "third_party/DirectXTex/DirectXTexFlipRotate.cpp",
         "third_party/DirectXTex/DirectXTexHDR.cpp",
         "third_party/DirectXTex/DirectXTexImage.cpp",
         "third_party/DirectXTex/DirectXTexMipMaps.cpp",
         "third_party/DirectXTex/DirectXTexMisc.cpp",
         "third_party/DirectXTex/DirectXTexNormalMaps.cpp",
         "third_party/DirectXTex/DirectXTexPMAlpha.cpp",
         "third_party/DirectXTex/DirectXTexResize.cpp",
         "third_party/DirectXTex/DirectXTexTGA.cpp",
         "third_party/DirectXTex/DirectXTexUtil.cpp",
         "third_party/DirectXTex/DirectXTexWIC.cpp"
      }

      includedirs {
         "third_party/DirectXTex",
         "third_party/DirectXTex/Common",
         "third_party/DirectXTex/Shaders/Compiled"
      }

      defines {
         "WIN32",
         "_LIB",
         "_UNICODE",
         "UNICODE",
         "_WIN32_WINNT=0x0603",
         "_CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS"
      }

      pchheader "DirectXTexP.h"
      pchsource "third_party/DirectXTex/DirectXTexUtil.cpp"

      openmp "On"
      floatingpoint "Fast"

   -- Project 2: DagorAssetsExtractorApp
   project "DagorAssetsExtractorApp"
      kind "WindowedApp"
      language "C++"
      location "DagorAssetsExtractorApp"
      targetdir "build/bin/%{cfg.buildcfg}"
      objdir "build/obj/%{cfg.buildcfg}/DagorAssetsExtractorApp"

      files {
         "DagorAssetsExtractorApp/src/**.h",
         "DagorAssetsExtractorApp/src/**.cpp",
         "third_party/adler32.c",
         "third_party/crc32.c",
         "third_party/tinflate.c",
         "third_party/tinfzlib.c",
         "third_party/zstddeclib.c",
         "third_party/LzmaDec.c",
         "third_party/tinf.h",
         "third_party/zstd.h",
         "third_party/zstd_errors.h",
         "third_party/LzmaDec.h",
         "third_party/7zTypes.h",
         "third_party/imgui/imgui.cpp",
         "third_party/imgui/imgui_demo.cpp",
         "third_party/imgui/imgui_draw.cpp",
         "third_party/imgui/imgui_tables.cpp",
         "third_party/imgui/imgui_widgets.cpp",
         "third_party/imgui/backends/imgui_impl_dx11.cpp",
         "third_party/imgui/backends/imgui_impl_win32.cpp",
         "third_party/imgui/imgui.h",
         "third_party/imgui/imgui_internal.h",
         "third_party/imgui/imconfig.h",
         "third_party/imgui/backends/imgui_impl_dx11.h",
         "third_party/imgui/backends/imgui_impl_win32.h"
      }

      includedirs {
         "DagorAssetsExtractorApp/src",
         "third_party",
         "third_party/DirectXTex",
         "third_party/imgui",
         "third_party/imgui/backends"
      }

      defines {
         "WIN32",
         "_WINDOWS",
         "UNICODE",
         "_UNICODE",
         "WIN32_LEAN_AND_MEAN",
         "NOMINMAX"
      }

      links {
         "DirectXTex",
         "d3d11.lib",
         "dxgi.lib",
         "d3dcompiler.lib"
      }

      postbuildcommands {
         "if exist \"..\\third_party\\oo2core_6_win64.dll\" copy /Y \"..\\third_party\\oo2core_6_win64.dll\" \"$(TargetDir)oo2core_6_win64.dll\" >nul",
         "robocopy \"..\\DagorAssetsExtractorApp\\assets\\ui\" \"$(TargetDir)assets\\ui\" /E /NFL /NDL /NJH /NJS /NP /XO || exit 0"
      }

   -- Project 3: AssetIndexTests
   project "AssetIndexTests"
      kind "ConsoleApp"
      language "C++"
      location "tests/AssetIndexTests"
      targetdir "build/bin/%{cfg.buildcfg}"
      objdir "build/obj/%{cfg.buildcfg}/AssetIndexTests"

      files {
         "tests/AssetIndexTests/main.cpp",
         "DagorAssetsExtractorApp/src/App/AppState.cpp",
         "DagorAssetsExtractorApp/src/App/AppState.h",
         "DagorAssetsExtractorApp/src/App/LogSystem.cpp",
         "DagorAssetsExtractorApp/src/App/LogSystem.h",
         "DagorAssetsExtractorApp/src/AssetIndex/AssetIndex.cpp",
         "DagorAssetsExtractorApp/src/AssetIndex/AssetIndex.h",
         "DagorAssetsExtractorApp/src/DagorIO/Decompression.cpp",
         "DagorAssetsExtractorApp/src/DagorIO/Decompression.h",
         "third_party/adler32.c",
         "third_party/crc32.c",
         "third_party/tinflate.c",
         "third_party/tinfzlib.c",
         "third_party/zstddeclib.c",
         "third_party/LzmaDec.c",
         "DagorAssetsExtractorApp/src/AssetIndex/BlkRelationships.cpp",
         "DagorAssetsExtractorApp/src/AssetIndex/BlkRelationships.h",
         "DagorAssetsExtractorApp/src/AssetIndex/DependencyGraph.cpp",
         "DagorAssetsExtractorApp/src/AssetIndex/DependencyGraph.h",
         "DagorAssetsExtractorApp/src/DagorIO/BinaryReader.cpp",
         "DagorAssetsExtractorApp/src/DagorIO/BinaryReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/VromfsReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/VromfsReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/DxpReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/DxpReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/GrpReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/GrpReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/Anim2DataReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/Anim2DataReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/DynModelReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/DynModelReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/DdsxReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/DdsxReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/BlkReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/BlkReader.h",
         "DagorAssetsExtractorApp/src/AssetIndex/PackContents.cpp",
         "DagorAssetsExtractorApp/src/AssetIndex/PackContents.h",
         "DagorAssetsExtractorApp/src/DagorFormats/GameResDescReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/GameResDescReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/GeomNodeTreeReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/GeomNodeTreeReader.h",
         "DagorAssetsExtractorApp/src/DagorFormats/RendInstReader.cpp",
         "DagorAssetsExtractorApp/src/DagorFormats/RendInstReader.h",
         "DagorAssetsExtractorApp/src/Export/ExportManifest.cpp",
         "DagorAssetsExtractorApp/src/Export/ExportManifest.h",
         "DagorAssetsExtractorApp/src/Export/TextureExporter.cpp",
         "DagorAssetsExtractorApp/src/Export/TextureExporter.h",
         "DagorAssetsExtractorApp/src/Export/ModelExporter.cpp",
         "DagorAssetsExtractorApp/src/Export/ModelExporter.h",
         "DagorAssetsExtractorApp/src/Jobs/JobSystem.cpp",
         "DagorAssetsExtractorApp/src/Jobs/JobSystem.h",
         "DagorAssetsExtractorApp/src/Platform/Platform.cpp",
         "DagorAssetsExtractorApp/src/Platform/Platform.h"
      }

      includedirs {
         "DagorAssetsExtractorApp/src",
         "third_party",
         "third_party/DirectXTex"
      }

      defines {
         "WIN32",
         "_CONSOLE",
         "UNICODE",
         "_UNICODE",
         "WIN32_LEAN_AND_MEAN",
         "NOMINMAX"
      }

      links {
         "DirectXTex"
      }
