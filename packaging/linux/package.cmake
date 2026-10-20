# cmake -DEXE_DIR=<folder with sst_rematch> -DSRC_DIR=<this folder> -DOUT_DIR=<where to put it> -DNAME=<package name> -P package.cmake
# Stages the Linux package (exe, its libraries, the shader packs and these files) as OUT_DIR/NAME and OUT_DIR/NAME.tar.gz.
set(_stage "${OUT_DIR}/${NAME}")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}/shaders")

file(GLOB _libs "${EXE_DIR}/*.so" "${EXE_DIR}/*.so.*")
list(FILTER _libs EXCLUDE REGEX "librexgpu-xenos")   # the native renderer replaces the emulated GPU
file(GLOB _packs "${EXE_DIR}/shaders/*.pack")
file(COPY "${EXE_DIR}/sst_rematch" ${_libs} DESTINATION "${_stage}" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(COPY ${_packs} DESTINATION "${_stage}/shaders")
file(COPY "${SRC_DIR}/sst_rematch.sh" "${SRC_DIR}/install-desktop-entry.sh" DESTINATION "${_stage}" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(COPY "${SRC_DIR}/README.txt" "${SRC_DIR}/sst_rematch.png" DESTINATION "${_stage}")

if(NOT _packs)
  message(WARNING "No shader packs in ${EXE_DIR}/shaders: the package will compile every shader on first launch")
endif()

# Debug info is most of the exe's size; the crash report only needs the symbol table and the guest function map
find_program(_strip strip)
if(_strip)
  file(GLOB _bins "${_stage}/sst_rematch" "${_stage}/*.so")
  foreach(_b ${_bins})
    execute_process(COMMAND "${_strip}" --strip-debug "${_b}")
  endforeach()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${NAME}.tar.gz" "${NAME}" WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE _r)
if(_r)
  message(FATAL_ERROR "tar failed: ${_r}")
endif()
message(STATUS "Linux package: ${OUT_DIR}/${NAME}.tar.gz")