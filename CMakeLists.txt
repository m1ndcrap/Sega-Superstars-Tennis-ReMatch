# sst_rematch - ReXGlue Recompiled Project

# SDK boilerplate lives in generated/rexglue.cmake

cmake_minimum_required(VERSION 3.25)
project(sst_rematch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SSTREMATCH_SOURCES
  src/main.cpp
  src/audio/silent_fallback.cpp
  src/gpu/d3d_trace.cpp
  src/menu/graphics_menu.cpp
  src/menu/video_settings.cpp
  src/menu/video_page.cpp
  src/menu/natives.cpp
  src/menu/controls_page.cpp
  src/menu/script_start.cpp
  src/menu/scene_files.cpp
  src/menu/menu_texts.cpp
  src/menu/camera_aspect.cpp
  src/menu/achievements.cpp
  src/menu/language.cpp
  src/menu/port_text.cpp
  src/menu/pause_patch.cpp
  src/menu/restart.cpp
  src/menu/hud_anchor.cpp
  src/menu/scene_format.cpp
  src/menu/scene_widen.cpp
  src/menu/scene_rows.cpp
  src/menu/scene_achievements.cpp
  src/menu/script_asm.cpp
  src/menu/main_menu_quit.cpp
  src/menu/vs_screen.cpp
  src/menu/shadow_aspect.cpp
  src/movie/movie_debug.cpp
  src/movie/movie_audio.cpp
  src/input/devices.cpp
  src/input/key_bindings.cpp
  src/game/court_collision.cpp
  src/debug/crash_log.cpp
  src/debug/scene_guard.cpp
  src/input/cursor.cpp
  src/input/lightbar.cpp
  src/window/window_fit.cpp
)

# The native renderer uses D3D12 or Vulkan on Windows, Vulkan on Linux
if(UNIX AND NOT APPLE AND REXGLUE_USE_VULKAN)
  set(SST_NATIVE_LINUX ON)
else()
  set(SST_NATIVE_LINUX OFF)
endif()

if(WIN32 OR SST_NATIVE_LINUX)
  # Game-thread capture (native, capture*, write_watch), render thread, renderer*
  list(APPEND SSTREMATCH_SOURCES
    src/gpu/native/native.cpp
    src/gpu/native/capture.cpp
    src/gpu/native/capture_textures.cpp
    src/gpu/native/capture_vertices.cpp
    src/gpu/native/capture_perf.cpp
    src/gpu/native/render_thread.cpp
    src/gpu/native/write_watch.cpp
    src/gpu/native/dxc.cpp
    src/gpu/native/renderer.cpp
    src/gpu/native/renderer_targets.cpp
    src/gpu/native/renderer_present.cpp
    src/gpu/native/renderer_pipelines.cpp
    src/gpu/native/renderer_draw.cpp
    src/gpu/native/renderer_vertices.cpp
    src/gpu/native/renderer_shaders.cpp
    src/gpu/native/renderer_constants.cpp
    src/gpu/native/renderer_textures.cpp
    src/gpu/native/renderer_geometry.cpp
    src/gpu/native/renderer_memexport.cpp
    src/gpu/native/renderer_profile.cpp
    src/gpu/native/renderer_debug.cpp
    src/gpu/native/shader_cache.cpp
    src/gpu/native/vertex_format.cpp
    src/gpu/native/memexport.cpp
    src/gpu/native/cpu_shader.cpp
    src/gpu/native/ucode_analysis.cpp
    src/gpu/native/xenos_ucode_tables.cpp
    src/gpu/native/texture_cache.cpp
    src/gpu/native/guest_watch.cpp
    src/gpu/native/hitch_profiler.cpp
    src/gpu/native/prompts.cpp
    src/gpu/native/prompt_image.cpp
    src/gpu/native/prompt_keys.cpp
  )

  # D3D12: its backend, the DXGI present hook (VSync), device-removed diagnostics
  if(WIN32)
    list(APPEND SSTREMATCH_SOURCES
      src/gpu/native/gfx/d3d12/d3d12_gfx.cpp
      src/gpu/native/present_hook.cpp
      src/gpu/d3d12_setup.cpp
    )
  endif()

  # Vulkan backend of the native renderer (sst_gpu_api = vulkan), with an SDK built with REXGLUE_USE_VULKAN
  if(REXGLUE_USE_VULKAN)
    list(APPEND SSTREMATCH_SOURCES src/gpu/native/gfx/vulkan/vulkan_gfx.cpp)
  endif()
else()
  list(APPEND SSTREMATCH_SOURCES src/gpu/native/native_stub.cpp)
endif()

if(WIN32)
  list(APPEND SSTREMATCH_SOURCES icon/icon.rc)
  set_source_files_properties(icon/icon.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.ico")
  add_executable(sst_rematch WIN32 ${SSTREMATCH_SOURCES})
else()
  add_executable(sst_rematch ${SSTREMATCH_SOURCES})
endif()

target_include_directories(sst_rematch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32 OR SST_NATIVE_LINUX)
  target_compile_definitions(sst_rematch PRIVATE SST_NATIVE_RENDERER=1)
endif()

# src/input/lightbar.cpp writes the PlayStation pads' HID output reports; src/window/window_fit.cpp subclasses the game window (comctl32)
if(WIN32)
  target_link_libraries(sst_rematch PRIVATE hid setupapi ole32 comctl32)
endif()

# Linker map next to the exe: turns the "sst_rematch.exe+0x..." frames of a crash report (sst_crash.txt) into function names
if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  target_link_options(sst_rematch PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:sst_rematch>/sst_rematch.map")
endif()

# src/input/devices.cpp builds the SDK's input drivers itself; sdl_input_driver.h includes SDL3's headers (types only - the driver code lives in rexruntime)
if(TARGET SDL3::SDL3)
  target_include_directories(sst_rematch PRIVATE $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>)
elseif(REXSDK_DIR)
  target_include_directories(sst_rematch PRIVATE ${REXSDK_DIR}/thirdparty/sdl3/include)
endif()

# This native renderer GPU plugin is the SDK's generic GPU front end (command buffer, registers, fences, interrupts, vsync, swap) with our backend, which draws nothing itself: the native renderer (src/gpu/native) records the game's D3D calls instead. It is the only GPU on Windows. The SDK's emulated GPU (rexgpu-xenos) is not built or shipped there; other platforms, which have no D3D12, still use it
if(WIN32 OR SST_NATIVE_LINUX)
  if(NOT REXSDK_DIR)
    message(FATAL_ERROR "sst_rematch: the native renderer's GPU plugin is built from the SDK sources; set REXSDK_DIR")
  endif()

  if(TARGET rexgpu-xenos)
    set_target_properties(rexgpu-xenos PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()

  set(SST_GPU_PLUGINS sst)
else()
  set(SST_GPU_PLUGINS xenos)
endif()

if(WIN32 OR SST_NATIVE_LINUX)
  set(_sdk_gpu "${REXSDK_DIR}/src/graphics")
  add_library(rexgpu-sst SHARED
    # SDK generic GPU sources (same list as rexgpu-xenos, minus its plugin_main.cpp and the emulation backends)
    ${_sdk_gpu}/flags.cpp
    ${_sdk_gpu}/register_file.cpp
    ${_sdk_gpu}/registers.cpp
    ${_sdk_gpu}/pipeline/shader/shader.cpp
    ${_sdk_gpu}/format/ucode.cpp
    ${_sdk_gpu}/sampler_info.cpp
    ${_sdk_gpu}/graphics_system.cpp
    ${_sdk_gpu}/command_processor.cpp
    ${_sdk_gpu}/util/draw_extent_estimator.cpp
    ${_sdk_gpu}/util/draw.cpp
    ${_sdk_gpu}/packet_disassembler.cpp
    ${_sdk_gpu}/primitive_processor.cpp
    ${_sdk_gpu}/pipeline/render_target/cache.cpp
    ${_sdk_gpu}/shared_memory.cpp
    ${_sdk_gpu}/pipeline/texture/cache.cpp
    ${_sdk_gpu}/pipeline/shader/interpreter.cpp
    ${_sdk_gpu}/pipeline/shader/translator.cpp
    ${_sdk_gpu}/pipeline/shader/translator_disasm.cpp
    # our backend
    src/gpu/plugin/sst_graphics_system.cpp
  )
  set_target_properties(rexgpu-sst PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Match rex::system::LoadGpuPlugin's per-config file names
    DEBUG_POSTFIX "d"
    RELWITHDEBINFO_POSTFIX "rd"
  )
  target_include_directories(rexgpu-sst PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc ${REXSDK_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(rexgpu-sst PRIVATE -Wno-unused-variable)

  # Same rule as rexgpu-xenos: link rexruntime only, never the OBJECT libs
  target_link_libraries(rexgpu-sst PRIVATE rex::runtime xxHash::xxhash)

  if(WIN32)
    target_link_libraries(rexgpu-sst PRIVATE d3d12 dxgi dxguid)
  endif()
  rexglue_apply_target_settings(rexgpu-sst)

  # Linker map next to the DLL (crash reports name rexgpu-sst.dll+0x... frames)
  if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    target_link_options(rexgpu-sst PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:rexgpu-sst>/rexgpu-sst.map")
  endif()
endif()

# Native renderer: shader translation
if(WIN32 OR SST_NATIVE_LINUX)
  # Vendored XenosRecomp translator (thirdparty/xenosrecomp/README.md)
  set(_xr "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/xenosrecomp")

  # shader_common.h doubles as the HLSL prelude of every translated shader: embed it as bytes
  set(_xr_gen "${CMAKE_CURRENT_BINARY_DIR}/sst_generated")
  file(READ "${_xr}/shader_common.h" _xr_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _xr_bytes "${_xr_hex}")
  file(MAKE_DIRECTORY "${_xr_gen}")
  file(CONFIGURE OUTPUT "${_xr_gen}/shader_common_hlsl.inc" CONTENT "${_xr_bytes}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xr}/shader_common.h")

  add_library(sst_xenosrecomp STATIC
    ${_xr}/shader_recompiler.cpp
    ${_xr}/sst_translate.cpp
  )
  target_include_directories(sst_xenosrecomp PUBLIC ${_xr} PRIVATE ${_xr_gen})
  target_compile_definitions(sst_xenosrecomp PRIVATE SST_RECOMP _CRT_SECURE_NO_WARNINGS)
  target_precompile_headers(sst_xenosrecomp PRIVATE ${_xr}/sst_prefix.h)
  target_compile_options(sst_xenosrecomp PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -Wno-unused-function)

  if(TARGET fmt::fmt)
    target_link_libraries(sst_xenosrecomp PRIVATE fmt::fmt)
  else()
    find_package(fmt REQUIRED)
    target_link_libraries(sst_xenosrecomp PRIVATE fmt::fmt)
  endif()

  target_link_libraries(sst_rematch PRIVATE sst_xenosrecomp xxHash::xxhash)
endif()

# Linux: the runtime shader compiler is libdxcompiler.so from a DirectXShaderCompiler release (SPIR-V), downloaded once (or taken from SST_DXC_SPIRV_DIR, the unpacked release) and copied next to the exe; its headers (dxcapi.h, WinAdapter.h) are used too
if(SST_NATIVE_LINUX)
  target_link_libraries(sst_rematch PRIVATE ${CMAKE_DL_LIBS})
  target_include_directories(sst_rematch PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
  set(SST_DXC_SPIRV_DIR "" CACHE PATH "Unpacked DirectXShaderCompiler release (SPIR-V back end) for the Vulkan renderer (default: downloaded into the build folder)")
  set(_dxs "${SST_DXC_SPIRV_DIR}")

  if(NOT _dxs)
    set(_dxs "${CMAKE_BINARY_DIR}/dxc-spirv")

    if(NOT EXISTS "${_dxs}/lib/libdxcompiler.so")
      set(_dxs_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505/linux_dxc_2025_05_24.x86_64.tar.gz")
      message(STATUS "sst_rematch: downloading DXC (SPIR-V) from ${_dxs_url}")
      file(DOWNLOAD "${_dxs_url}" "${CMAKE_BINARY_DIR}/dxc-spirv.tar.gz" EXPECTED_HASH SHA256=b99655f65215287825fcdd49102b17e2a1608eff79ffaf9457514c2676892aa5 STATUS _dxs_status)
      list(GET _dxs_status 0 _dxs_code)

      if(_dxs_code EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/dxc-spirv.tar.gz" DESTINATION "${_dxs}")
      endif()
    endif()
  endif()

  if(EXISTS "${_dxs}/lib/libdxcompiler.so" AND EXISTS "${_dxs}/include/dxc/dxcapi.h")
    target_include_directories(sst_rematch PRIVATE "${_dxs}/include/dxc")
    set(_dxs_bin "${_dxs}/lib")
    add_custom_command(TARGET sst_rematch POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxs}/lib/libdxcompiler.so" "$<TARGET_FILE_DIR:sst_rematch>"
      VERBATIM
    )
  else()
    message(FATAL_ERROR "sst_rematch: no DXC release for the Vulkan renderer (headers + libdxcompiler.so). Set SST_DXC_SPIRV_DIR to an unpacked linux_dxc_*.tar.gz from https://github.com/microsoft/DirectXShaderCompiler/releases")
  endif()
endif()

# Windows: D3D12, the Windows SDK's DXC (DXIL) and the build-time shader pack
if(WIN32)
  target_link_libraries(sst_rematch PRIVATE d3d12 dxgi dxguid cabinet)

  if(TARGET dxc-headers)
    target_link_libraries(sst_rematch PRIVATE dxc-headers)
  endif()

  # Runtime shader compiler: dxcompiler.dll + dxil.dll (dxil.dll signs the DXIL; D3D12 rejects unsigned shaders). Taken from the Windows SDK unless SST_DXC_DIR points elsewhere
  set(SST_DXC_DIR "" CACHE PATH "Folder with dxcompiler.dll and dxil.dll (default: Windows SDK)")
  set(_dxc_dir "${SST_DXC_DIR}")

  if(NOT _dxc_dir AND DEFINED ENV{WindowsSdkVerBinPath} AND EXISTS "$ENV{WindowsSdkVerBinPath}/x64/dxcompiler.dll")
    set(_dxc_dir "$ENV{WindowsSdkVerBinPath}/x64")
  endif()

  if(NOT _dxc_dir)
    file(GLOB _dxc_found "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll")

    if(_dxc_found)
      list(SORT _dxc_found COMPARE NATURAL ORDER DESCENDING)
      list(GET _dxc_found 0 _dxc_first)
      get_filename_component(_dxc_dir "${_dxc_first}" DIRECTORY)
    endif()
  endif()

  if(_dxc_dir AND EXISTS "${_dxc_dir}/dxcompiler.dll" AND EXISTS "${_dxc_dir}/dxil.dll")
    file(TO_CMAKE_PATH "${_dxc_dir}" _dxc_dir)
    message(STATUS "sst_rematch: shader compiler DLLs from ${_dxc_dir}")
    add_custom_command(TARGET sst_rematch POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxc_dir}/dxcompiler.dll" "${_dxc_dir}/dxil.dll" "$<TARGET_FILE_DIR:sst_rematch>"
      VERBATIM
    )
  else()
    message(WARNING "sst_rematch: dxcompiler.dll/dxil.dll not found; set SST_DXC_DIR. " "The native renderer can't compile new shaders without them.")
  endif()

  # Vulkan: shaders compile to SPIR-V, which the Windows SDK's DXC can't produce. A DirectXShaderCompiler release can as it is downloaded once (or taken from SST_DXC_SPIRV_DIR, the unpacked release) and its dxcompiler.dll is copied next to the exe as dxcompiler-spirv.dll
  if(REXGLUE_USE_VULKAN)
    # rex/ui/vulkan/provider.h includes the SDK's renderdoc header (its include path isn't exported)
    if(REXSDK_DIR)
      target_include_directories(sst_rematch PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
    endif()

    set(SST_DXC_SPIRV_DIR "" CACHE PATH "Unpacked DirectXShaderCompiler release (SPIR-V back end) for the Vulkan renderer (default: downloaded into the build folder)")
    set(_dxs "${SST_DXC_SPIRV_DIR}")

    if(NOT _dxs)
      set(_dxs "${CMAKE_BINARY_DIR}/dxc-spirv")

      if(NOT EXISTS "${_dxs}/bin/x64/dxcompiler.dll")
        set(_dxs_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505/dxc_2025_05_24.zip")
        message(STATUS "sst_rematch: downloading DXC (SPIR-V) from ${_dxs_url}")
        file(DOWNLOAD "${_dxs_url}" "${CMAKE_BINARY_DIR}/dxc-spirv.zip" EXPECTED_HASH SHA256=81380f3eca156d902d6404fd6df9f4b0886f576ff3e18b2cc10d3075ffc9d119 STATUS _dxs_status)
        list(GET _dxs_status 0 _dxs_code)

        if(_dxs_code EQUAL 0)
          file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/dxc-spirv.zip" DESTINATION "${_dxs}")
        endif()
      endif()
    endif()

    if(EXISTS "${_dxs}/bin/x64/dxcompiler.dll")
      set(_dxs_bin "${_dxs}/bin/x64")
      add_custom_command(TARGET sst_rematch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxs}/bin/x64/dxcompiler.dll" "$<TARGET_FILE_DIR:sst_rematch>/dxcompiler-spirv.dll"
        VERBATIM
      )
    else()
      message(WARNING "sst_rematch: no SPIR-V shader compiler (DXC release) - the Vulkan renderer can't compile shaders. Set SST_DXC_SPIRV_DIR to an unpacked release from https://github.com/microsoft/DirectXShaderCompiler/releases")
    endif()
  endif()

endif()

# Build-time shader compilation, for D3D12 (Windows) and Vulkan (Windows with REXGLUE_USE_VULKAN, Linux)
if(WIN32 OR SST_NATIVE_LINUX)
  # Build-time shader compilation (tools/shader_bake): translates and compiles every shader in the game's ShaderCache files (and every spec variant the renderer can ask for) during the build, and embeds the DXIL in the exe as RCDATA "SST_SHADER_PACK". Players never compile these shaders: no first-launch stutter and no missing materials while a slow CPU catches up. The pack is built from your own game files, like the recompiled code itself. Shaders the game generates at runtime (not in any game file) still compile on first use, ahead of everything else
  option(SST_SHADER_BAKE "Precompile the game's shaders at build time and embed them in the exe" ON)

  # VIDEO > SHADER PRECISION: FAST uses plain IEEE multiplies instead of the Xenos/D3D9 rule (0 * anything = 0), ~2-3 ms less GPU time a frame on integrated GPUs. It needs a second pack (~40 MB more exe, about twice the bake time on a clean build). Off: FAST falls back to ACCURATE
  option(SST_SHADER_BAKE_FAST "Also bake the SHADER PRECISION: FAST shader pack" ON)
  set(SST_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Extracted game (the folder holding default.xex): source of the ShaderCache files")
  set(SST_SHADER_DUMP_DIR "" CACHE PATH "Optional: more raw shader containers to bake, e.g. --sst_dump_shaders output (<exe>/d3d_trace/shaders) or another PC's cache/containers")

  # Shaders the game generates while running exist in no game file. The exe saves each one it had to compile to <exe>/cache/containers; the next build bakes them in. Play through the characters, courts and modes once with the exe in the build folder, then rebuild
  set(SST_SHADER_HARVEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/cache/containers" CACHE PATH "Shaders the built exe saved because they weren't in its pack (<exe>/cache/containers); baked into the next build")

  add_executable(sst_shader_bake tools/shader_bake/shader_bake.cpp)
  target_include_directories(sst_shader_bake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(sst_shader_bake PRIVATE sst_xenosrecomp xxHash::xxhash)

  if(WIN32)
    target_link_libraries(sst_shader_bake PRIVATE cabinet)

    if(TARGET dxc-headers)
      target_link_libraries(sst_shader_bake PRIVATE dxc-headers)
    endif()

    # The tool runs during the build: put any DLLs it links (fmt, xxHash in shared SDK builds) next to it
    set(_bake_dlls "$<TARGET_RUNTIME_DLLS:sst_shader_bake>")
    add_custom_command(TARGET sst_shader_bake POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:${_bake_dlls}>,copy_if_different,true> ${_bake_dlls} $<$<BOOL:${_bake_dlls}>:$<TARGET_FILE_DIR:sst_shader_bake>>
      COMMAND_EXPAND_LISTS
    )
  else()
    target_link_libraries(sst_shader_bake PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(sst_shader_bake PRIVATE "${_dxs}/include/dxc")
  endif()

  set(_bake_inputs "")
  set(_bake_args "")

  if(SST_SHADER_BAKE AND EXISTS "${SST_GAME_DIR}")
    file(GLOB_RECURSE _bake_game_files "${SST_GAME_DIR}/*code_cache*.bin")

    if(_bake_game_files)
      list(APPEND _bake_inputs ${_bake_game_files})
      list(APPEND _bake_args --game "${SST_GAME_DIR}")
    endif()
  endif()

  foreach(_dump_dir IN ITEMS "${SST_SHADER_HARVEST_DIR}" "${SST_SHADER_DUMP_DIR}")
    if(SST_SHADER_BAKE AND _dump_dir)
      # CONFIGURE_DEPENDS: newly saved shaders re-run CMake and the bake on the next build
      file(GLOB_RECURSE _bake_dump_files CONFIGURE_DEPENDS "${_dump_dir}/*.bin")

      if(_bake_dump_files)
        list(LENGTH _bake_dump_files _dump_count)
        message(STATUS "sst_rematch: also baking ${_dump_count} saved shader(s) from ${_dump_dir}")
        list(APPEND _bake_inputs ${_bake_dump_files})
        list(APPEND _bake_args --dump "${_dump_dir}")
      endif()
    endif()
  endforeach()

  # DXIL packs (D3D12), embedded in the exe
  if(WIN32)
    if(NOT SST_SHADER_BAKE)
      message(STATUS "sst_rematch: shader baking off (SST_SHADER_BAKE=OFF): shaders compile on first use")
    elseif(NOT _bake_inputs)
      message(WARNING "sst_rematch: no ShaderCache files (*code_cache*.bin) under ${SST_GAME_DIR}: " "building without precompiled shaders. Extract the game there (or set SST_GAME_DIR) and re-run CMake.")
    elseif(NOT _dxc_dir OR NOT EXISTS "${_dxc_dir}/dxil.dll")
      message(WARNING "sst_rematch: shader baking needs dxcompiler.dll + dxil.dll (SST_DXC_DIR): building without precompiled shaders")
    else()
      list(LENGTH _bake_inputs _bake_count)
      message(STATUS "sst_rematch: baking shaders from ${_bake_count} file(s) into the exe")
      set(_pack "${_xr_gen}/sst_shaders.pack")
      add_custom_command(
        OUTPUT "${_pack}"
        COMMAND sst_shader_bake --out "${_pack}" --dxc "${_dxc_dir}" --work "${CMAKE_CURRENT_BINARY_DIR}/shader_bake_work" ${_bake_args}
        DEPENDS sst_shader_bake ${_bake_inputs}
        COMMENT "Compiling the game's shaders (sst_shader_bake)"
        VERBATIM
      )
      set(_packs "${_pack}")
      set(_pack_rc "SST_SHADER_PACK RCDATA \"${_pack}\"\n")

      if(SST_SHADER_BAKE_FAST)
        set(_pack_fast "${_xr_gen}/sst_shaders_fast.pack")
        add_custom_command(
          OUTPUT "${_pack_fast}"
          COMMAND sst_shader_bake --fast --out "${_pack_fast}" --dxc "${_dxc_dir}" --work "${CMAKE_CURRENT_BINARY_DIR}/shader_bake_work_fast" ${_bake_args}
          DEPENDS sst_shader_bake ${_bake_inputs}
          COMMENT "Compiling the game's shaders, SHADER PRECISION: FAST (sst_shader_bake --fast)"
          VERBATIM
        )
        list(APPEND _packs "${_pack_fast}")
        string(APPEND _pack_rc "SST_SHADER_PACK_FAST RCDATA \"${_pack_fast}\"\n")
      endif()

      add_custom_target(sst_shader_pack DEPENDS ${_packs})
      file(CONFIGURE OUTPUT "${_xr_gen}/sst_shader_pack.rc" CONTENT "${_pack_rc}")
      target_sources(sst_rematch PRIVATE "${_xr_gen}/sst_shader_pack.rc")
      set_source_files_properties("${_xr_gen}/sst_shader_pack.rc" PROPERTIES OBJECT_DEPENDS "${_packs}")
      add_dependencies(sst_rematch sst_shader_pack)
    endif()
  endif()

  # SPIR-V packs (Vulkan): the same shaders for the Vulkan renderer, one module per shader (variants are specialization constants), as files next to the exe (shaders/spirv.pack, shaders/spirv_fast.pack). Without them the Vulkan renderer compiles shaders on first use
  if(REXGLUE_USE_VULKAN AND SST_SHADER_BAKE AND _bake_inputs AND _dxs_bin)
    set(_spv_packs "${_xr_gen}/spirv.pack")
    add_custom_command(
      OUTPUT "${_xr_gen}/spirv.pack"
      COMMAND sst_shader_bake --spirv --out "${_xr_gen}/spirv.pack" --dxc "${_dxs_bin}" --work "${CMAKE_CURRENT_BINARY_DIR}/shader_bake_work_spirv" ${_bake_args}
      DEPENDS sst_shader_bake ${_bake_inputs}
      COMMENT "Compiling the game's shaders for Vulkan (sst_shader_bake --spirv)"
      VERBATIM
    )

    if(SST_SHADER_BAKE_FAST)
      list(APPEND _spv_packs "${_xr_gen}/spirv_fast.pack")
      add_custom_command(
        OUTPUT "${_xr_gen}/spirv_fast.pack"
        COMMAND sst_shader_bake --spirv --fast --out "${_xr_gen}/spirv_fast.pack" --dxc "${_dxs_bin}" --work "${CMAKE_CURRENT_BINARY_DIR}/shader_bake_work_spirv_fast" ${_bake_args}
        DEPENDS sst_shader_bake ${_bake_inputs}
        COMMENT "Compiling the game's shaders for Vulkan, SHADER PRECISION: FAST (sst_shader_bake --spirv --fast)"
        VERBATIM
      )
    endif()

    add_custom_target(sst_shader_pack_spirv DEPENDS ${_spv_packs})
    add_dependencies(sst_rematch sst_shader_pack_spirv)
    add_custom_command(TARGET sst_rematch POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sst_rematch>/shaders"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_spv_packs} "$<TARGET_FILE_DIR:sst_rematch>/shaders"
      VERBATIM
    )
    message(STATUS "sst_rematch: baking SPIR-V shader packs for the Vulkan renderer (next to the exe, in shaders/)")
  endif()
endif()

# Recompiled CPU code + GPU plugins staged next to the exe
rexglue_setup_target(sst_rematch GPU_PLUGINS ${SST_GPU_PLUGINS})

# The native renderer's Vulkan backend needs features the SDK's Vulkan device doesn't enable on its own (buffer device addresses, bindless descriptor arrays): rexglue-sst.patch at the repo root adds them. With the SDK from source (REXSDK_DIR), warn when it isn't applied. (An installed SDK package must be built from a patched source tree)
if(REXSDK_DIR AND EXISTS "${REXSDK_DIR}/src/ui/vulkan/vulkan_device.cpp")
  file(READ "${REXSDK_DIR}/src/ui/vulkan/vulkan_device.cpp" _sst_vk_device)
  string(FIND "${_sst_vk_device}" "sst_rematch native renderer" _sst_vk_patched)

  if(_sst_vk_patched EQUAL -1)
    message(WARNING "sst_rematch: the ReXGlue SDK at ${REXSDK_DIR} is missing rexglue-sst.patch (Vulkan features for the native renderer). Apply it with:\n  git -C \"${REXSDK_DIR}\" apply \"${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sst.patch\"")
  endif()
endif()

# Linux: the exe finds its shared libraries next to itself ($ORIGIN), but the SDK stages them only on Windows and macOS. Copy librexruntime.so and libTracyClient.so there too, so the build folder runs as it is
if(UNIX AND NOT APPLE)
  foreach(_sst_lib rex::runtime rexruntime rex::TracyClient TracyClient)
    if(TARGET ${_sst_lib})
      get_target_property(_sst_lib_type ${_sst_lib} TYPE)

      if(_sst_lib_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET sst_rematch POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${_sst_lib}> $<TARGET_FILE_DIR:sst_rematch>)
      endif()
    endif()
  endforeach()
endif()

# Linux package: `cmake --build <build> --target sst_package_linux` stages <build>/package/sst_rematch_linux (the exe, its libraries, the shader packs, a launcher and a menu-entry script; never game data) and packs it as sst_rematch_linux.tar.gz next to it
if(SST_NATIVE_LINUX)
  add_custom_target(sst_package_linux
    COMMAND ${CMAKE_COMMAND} -DEXE_DIR=$<TARGET_FILE_DIR:sst_rematch> -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux
            -DOUT_DIR=${CMAKE_BINARY_DIR}/package -DNAME=sst_rematch_linux -P ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/package.cmake
    DEPENDS sst_rematch
    COMMENT "Packaging the Linux build"
    VERBATIM
  )
endif()

# SDK-from-source (REXSDK_DIR) builds don't propagate imgui's include path, but rex/rex_app.h includes <imgui.h>. Headers only: imgui's code already lives in rexruntime, and linking imgui::imgui here would add a second copy
if(TARGET imgui)
  target_include_directories(sst_rematch PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()