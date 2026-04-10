if(NOT DEFINED REF_PLAYER_PLUGIN_DIR OR REF_PLAYER_PLUGIN_DIR STREQUAL "")
  message(FATAL_ERROR "REF_PLAYER_PLUGIN_DIR is required")
endif()

if(NOT DEFINED CUSTOM_PLUGIN OR CUSTOM_PLUGIN STREQUAL "")
  message(FATAL_ERROR "CUSTOM_PLUGIN is required")
endif()

if(NOT DEFINED OUT_DIR OR OUT_DIR STREQUAL "")
  message(FATAL_ERROR "OUT_DIR is required")
endif()

if(NOT EXISTS "${CUSTOM_PLUGIN}")
  message(FATAL_ERROR "Custom plugin does not exist: ${CUSTOM_PLUGIN}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

file(GLOB reference_plugins "${REF_PLAYER_PLUGIN_DIR}/*.dylib")

foreach(plugin IN LISTS reference_plugins)
  get_filename_component(plugin_name "${plugin}" NAME)
  if(NOT plugin_name STREQUAL "libdlboamdmod.dylib")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E create_symlink "${plugin}" "${OUT_DIR}/${plugin_name}"
      COMMAND_ERROR_IS_FATAL ANY
    )
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CUSTOM_PLUGIN}" "${OUT_DIR}/libdlboamdmod.dylib"
  COMMAND_ERROR_IS_FATAL ANY
)

message(STATUS "Prepared Reference Player plugin directory: ${OUT_DIR}")
