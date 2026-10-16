# sst_rematch - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sst_rematch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SSTREMATCH_SOURCES
    src/main.cpp
    src/gpu/d3d_trace.cpp
    src/menu/graphics_menu.cpp
    src/menu/achievements.cpp
    src/menu/pause_patch.cpp
    src/menu/restart.cpp
    src/menu/hud_anchor.cpp
    src/menu/scene_widen.cpp
    src/menu/main_menu_quit.cpp
    src/movie/movie_debug.cpp
    src/input/controls.cpp
    src/debug/crash_log.cpp
    src/debug/scene_guard.cpp
    src/input/cursor.cpp
    src/input/lightbar.cpp
)
if(WIN32)
    # Native D3D12 renderer (active with --gpu_plugin=sst)
    list(APPEND SSTREMATCH_SOURCES
        src/gpu/native/native.cpp
        src/gpu/native/renderer.cpp
        src/gpu/native/shader_cache.cpp
        src/gpu/native/vertex_format.cpp
        src/gpu/native/memexport.cpp
        src/gpu/native/xenos_ucode_tables.cpp
        src/gpu/native/texture_cache.cpp
        src/gpu/native/hitch_profiler.cpp
        src/gpu/native/present_hook.cpp
        src/gpu/native/prompts.cpp
        src/gpu/d3d12_setup.cpp
    )
else()
    list(APPEND SSTREMATCH_SOURCES src/gpu/native/native_stub.cpp)
endif()

if(WIN32)
    list(APPEND SSTREMATCH_SOURCES icon/icon.rc)
    add_executable(sst_rematch WIN32 ${SSTREMATCH_SOURCES})
else()
    add_executable(sst_rematch ${SSTREMATCH_SOURCES})
endif()
target_include_directories(sst_rematch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# src/input/lightbar.cpp writes the PlayStation pads' HID output reports
if(WIN32)
    target_link_libraries(sst_rematch PRIVATE hid setupapi)
endif()
# Linker map next to the exe: turns the "sst_rematch.exe+0x..." frames of a crash report (sst_crash.txt) into function names
if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    target_link_options(sst_rematch PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:sst_rematch>/sst_rematch.map")
endif()

# src/input/controls.cpp builds the SDK's input drivers itself; sdl_input_driver.h includes SDL3's headers (types only - the driver code lives in rexruntime)
if(TARGET SDL3::SDL3)
    target_include_directories(sst_rematch PRIVATE $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>)
elseif(REXSDK_DIR)
    target_include_directories(sst_rematch PRIVATE ${REXSDK_DIR}/thirdparty/sdl3/include)
endif()

# --- "sst" GPU plugin: native rendering backend (Windows / D3D12) -------------
# Built like the SDK's rexgpu-xenos plugin: the SDK's generic GPU sources (PM4,
# registers, fences, interrupts, vsync, swap) plus our backend. Needs the SDK
# source tree (REXSDK_DIR). Select at runtime with --gpu_plugin=sst; xenos stays
# the default until native rendering is complete.
set(SST_GPU_PLUGINS xenos)
if(WIN32 AND REXSDK_DIR AND TARGET rexgpu-xenos)
    set(_sdk_gpu "${REXSDK_DIR}/src/graphics")
    add_library(rexgpu-sst SHARED
        # SDK generic GPU sources (same list as rexgpu-xenos, minus its
        # plugin_main.cpp and the emulation backends)
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
        # Match rex::system::LoadGpuPlugin's per-config file names.
        DEBUG_POSTFIX "d"
        RELWITHDEBINFO_POSTFIX "rd"
    )
    target_include_directories(rexgpu-sst PRIVATE
        ${REXSDK_DIR}/thirdparty/renderdoc
        ${REXSDK_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(rexgpu-sst PRIVATE -Wno-unused-variable)
    # Same rule as rexgpu-xenos: link rexruntime only, never the OBJECT libs.
    target_link_libraries(rexgpu-sst PRIVATE rex::runtime xxHash::xxhash d3d12 dxgi dxguid)
    rexglue_apply_target_settings(rexgpu-sst)
    # Linker map next to the DLL (crash reports name rexgpu-sst.dll+0x... frames)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_link_options(rexgpu-sst PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:rexgpu-sst>/rexgpu-sst.map")
    endif()
    list(APPEND SST_GPU_PLUGINS sst)
endif()

# --- Native renderer: shader translation (Windows) -----------------------------
if(WIN32)
    # Vendored XenosRecomp translator (thirdparty/xenosrecomp/README.md).
    set(_xr "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/xenosrecomp")
    # shader_common.h doubles as the HLSL prelude of every translated shader:
    # embed it as bytes.
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
    target_compile_options(sst_xenosrecomp PRIVATE
        -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -Wno-unused-function)
    if(TARGET fmt::fmt)
        target_link_libraries(sst_xenosrecomp PRIVATE fmt::fmt)
    else()
        find_package(fmt REQUIRED)
        target_link_libraries(sst_xenosrecomp PRIVATE fmt::fmt)
    endif()

    target_link_libraries(sst_rematch PRIVATE sst_xenosrecomp xxHash::xxhash d3d12 dxgi dxguid cabinet)
    if(TARGET dxc-headers)
        target_link_libraries(sst_rematch PRIVATE dxc-headers)
    endif()

    # Runtime shader compiler: dxcompiler.dll + dxil.dll (dxil.dll signs the
    # DXIL; D3D12 rejects unsigned shaders). Taken from the Windows SDK unless
    # SST_DXC_DIR points elsewhere.
    set(SST_DXC_DIR "" CACHE PATH "Folder with dxcompiler.dll and dxil.dll (default: Windows SDK)")
    set(_dxc_dir "${SST_DXC_DIR}")
    if(NOT _dxc_dir AND DEFINED ENV{WindowsSdkVerBinPath}
       AND EXISTS "$ENV{WindowsSdkVerBinPath}/x64/dxcompiler.dll")
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
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dxc_dir}/dxcompiler.dll" "${_dxc_dir}/dxil.dll"
                "$<TARGET_FILE_DIR:sst_rematch>"
            VERBATIM)
    else()
        message(WARNING "sst_rematch: dxcompiler.dll/dxil.dll not found; set SST_DXC_DIR. "
                        "The native renderer can't compile new shaders without them.")
    endif()

    # --- Build-time shader compilation (tools/shader_bake) ----------------------
    # Translates and compiles every shader in the game's ShaderCache files (and
    # every spec variant the renderer can ask for) during the build, and embeds
    # the DXIL in the exe as RCDATA "SST_SHADER_PACK". Players never compile
    # these shaders: no first-launch stutter and no missing materials while a
    # slow CPU catches up. The pack is built from your own game files, like the
    # recompiled code itself. Shaders the game generates at runtime (not in any
    # game file) still compile on first use, ahead of everything else.
    option(SST_SHADER_BAKE "Precompile the game's shaders at build time and embed them in the exe" ON)
    set(SST_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH
        "Extracted game (the folder holding default.xex): source of the ShaderCache files")
    set(SST_SHADER_DUMP_DIR "" CACHE PATH
        "Optional: more raw shader containers to bake, e.g. --sst_dump_shaders output (<exe>/d3d_trace/shaders) or another PC's cache/containers")
    # Shaders the game generates while running exist in no game file. The exe saves each one it had to compile to
    # <exe>/cache/containers; the next build bakes them in. Play through the characters, courts and modes once with
    # the exe in the build folder, then rebuild.
    set(SST_SHADER_HARVEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/cache/containers" CACHE PATH
        "Shaders the built exe saved because they weren't in its pack (<exe>/cache/containers); baked into the next build")

    add_executable(sst_shader_bake tools/shader_bake/shader_bake.cpp)
    target_include_directories(sst_shader_bake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(sst_shader_bake PRIVATE sst_xenosrecomp xxHash::xxhash cabinet)
    if(TARGET dxc-headers)
        target_link_libraries(sst_shader_bake PRIVATE dxc-headers)
    endif()
    # The tool runs during the build: put any DLLs it links (fmt, xxHash in shared SDK builds) next to it
    set(_bake_dlls "$<TARGET_RUNTIME_DLLS:sst_shader_bake>")
    add_custom_command(TARGET sst_shader_bake POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:${_bake_dlls}>,copy_if_different,true>
                ${_bake_dlls} $<$<BOOL:${_bake_dlls}>:$<TARGET_FILE_DIR:sst_shader_bake>>
        COMMAND_EXPAND_LISTS)

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

    if(NOT SST_SHADER_BAKE)
        message(STATUS "sst_rematch: shader baking off (SST_SHADER_BAKE=OFF): shaders compile on first use")
    elseif(NOT _bake_inputs)
        message(WARNING "sst_rematch: no ShaderCache files (*code_cache*.bin) under ${SST_GAME_DIR}: "
                        "building without precompiled shaders. Extract the game there (or set SST_GAME_DIR) and re-run CMake.")
    elseif(NOT _dxc_dir OR NOT EXISTS "${_dxc_dir}/dxil.dll")
        message(WARNING "sst_rematch: shader baking needs dxcompiler.dll + dxil.dll (SST_DXC_DIR): building without precompiled shaders")
    else()
        list(LENGTH _bake_inputs _bake_count)
        message(STATUS "sst_rematch: baking shaders from ${_bake_count} file(s) into the exe")
        set(_pack "${_xr_gen}/sst_shaders.pack")
        add_custom_command(
            OUTPUT "${_pack}"
            COMMAND sst_shader_bake --out "${_pack}" --dxc "${_dxc_dir}"
                    --work "${CMAKE_CURRENT_BINARY_DIR}/shader_bake_work" ${_bake_args}
            DEPENDS sst_shader_bake ${_bake_inputs}
            COMMENT "Compiling the game's shaders (sst_shader_bake)"
            VERBATIM)
        add_custom_target(sst_shader_pack DEPENDS "${_pack}")
        file(CONFIGURE OUTPUT "${_xr_gen}/sst_shader_pack.rc"
             CONTENT "SST_SHADER_PACK RCDATA \"${_pack}\"\n")
        target_sources(sst_rematch PRIVATE "${_xr_gen}/sst_shader_pack.rc")
        set_source_files_properties("${_xr_gen}/sst_shader_pack.rc" PROPERTIES OBJECT_DEPENDS "${_pack}")
        add_dependencies(sst_rematch sst_shader_pack)
    endif()
endif()

# Recompiled CPU code + GPU plugins staged next to the exe.
rexglue_setup_target(sst_rematch GPU_PLUGINS ${SST_GPU_PLUGINS})

# SDK-from-source (REXSDK_DIR) builds don't propagate imgui's include path, but
# rex/rex_app.h includes <imgui.h>. Headers only: imgui's code already lives in
# rexruntime, and linking imgui::imgui here would add a second copy.
if(TARGET imgui)
    target_include_directories(sst_rematch PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
