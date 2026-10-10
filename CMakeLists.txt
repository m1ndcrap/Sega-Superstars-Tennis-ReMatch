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
)
if(WIN32)
    # Native D3D12 renderer (active with --gpu_plugin=sst)
    list(APPEND SSTREMATCH_SOURCES
        src/gpu/native/native.cpp
        src/gpu/native/shader_cache.cpp
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

    target_link_libraries(sst_rematch PRIVATE sst_xenosrecomp xxHash::xxhash d3d12 dxgi dxguid)
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
