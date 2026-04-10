cmake_minimum_required(VERSION 3.24)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Target macOS architectures")
endif()

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "Minimum macOS deployment target")
endif()

project(gst_plugin_oamd_open VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(REF_PLAYER_GST_ROOT
    "/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources"
    CACHE PATH "Reference Player GStreamer runtime root")
set(REF_PLAYER_PLUGIN_DIR
    "/Applications/Dolby/Dolby Reference Player.app/Contents/PlugIns/gst-plugins"
    CACHE PATH "Reference Player plugin directory")
set(GLAZE_VERSION "v7.3.2" CACHE STRING "Glaze release tag to fetch")
set(GLAZE_URL_HASH
    "SHA256=4c8ee3039768eb67cee5335e052deafee4791bab87e3d4a965cce90e5c5c9f55"
    CACHE STRING "Expected hash for the Glaze release tarball")
set(GLAZE_SOURCE_DIR ""
    CACHE PATH "Optional local Glaze source checkout to use instead of downloading")
set(GST_HOME_AUDIO_GIT_REPOSITORY
    "https://github.com/DolbyLaboratories/gst-home-audio"
    CACHE STRING "gst-home-audio repository URL")
set(GST_HOME_AUDIO_GIT_TAG
    "e37c7a61b82cd91e4584ce1fe579c326991334e4"
    CACHE STRING "gst-home-audio commit or tag to fetch")
set(GST_HOME_AUDIO_SOURCE_DIR ""
    CACHE PATH "Optional local gst-home-audio checkout to use instead of downloading")
set(GST_HOME_AUDIO_VENDOR_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gst-home-audio"
    CACHE PATH "Directory where gst-home-audio should be populated")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0)

set(REF_PLAYER_GST_LIB_DIR "${REF_PLAYER_GST_ROOT}/lib")
set(REF_PLAYER_GST_SCANNER "${REF_PLAYER_GST_ROOT}/libexec/gst-plugin-scanner")
set(REF_PLAYER_GST_BIN_DIR "${REF_PLAYER_GST_ROOT}/bin")
set(REFERENCE_PLAYER_PLUGINS_DIR "${CMAKE_BINARY_DIR}/reference-player-plugins")

find_library(GST_AUDIO_LIBRARY
             NAMES gstaudio-1.0
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(GST_BASE_LIBRARY
             NAMES gstbase-1.0
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(GSTREAMER_LIBRARY
             NAMES gstreamer-1.0
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(GOBJECT_LIBRARY
             NAMES gobject-2.0
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(GLIB_LIBRARY
             NAMES glib-2.0
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(INTL_LIBRARY
             NAMES intl
             PATHS "${REF_PLAYER_GST_LIB_DIR}"
             NO_DEFAULT_PATH
             REQUIRED)
find_library(ICONV_LIBRARY NAMES iconv REQUIRED)

set(glaze_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(glaze_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(glaze_ENABLE_SSL OFF CACHE BOOL "" FORCE)
set(glaze_EETF_FORMAT OFF CACHE BOOL "" FORCE)

if(GLAZE_SOURCE_DIR)
  set(FETCHCONTENT_SOURCE_DIR_GLAZE "${GLAZE_SOURCE_DIR}")
endif()

FetchContent_Declare(
  glaze
  URL "https://github.com/stephenberry/glaze/archive/refs/tags/${GLAZE_VERSION}.tar.gz"
  URL_HASH "${GLAZE_URL_HASH}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glaze)

if(GST_HOME_AUDIO_SOURCE_DIR)
  set(GST_HOME_AUDIO_DIR "${GST_HOME_AUDIO_SOURCE_DIR}")
else()
  set(GST_HOME_AUDIO_DIR "${GST_HOME_AUDIO_VENDOR_DIR}")
  if(NOT EXISTS "${GST_HOME_AUDIO_DIR}/libs/meta/dlbaudiometa.h")
    FetchContent_Populate(
      gst_home_audio
      GIT_REPOSITORY "${GST_HOME_AUDIO_GIT_REPOSITORY}"
      GIT_TAG "${GST_HOME_AUDIO_GIT_TAG}"
      GIT_PROGRESS TRUE
      GIT_SHALLOW FALSE
      SOURCE_DIR "${GST_HOME_AUDIO_DIR}"
      SUBBUILD_DIR "${CMAKE_BINARY_DIR}/_deps/gst-home-audio-subbuild"
      BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/gst-home-audio-build"
      UPDATE_DISCONNECTED TRUE
    )
  endif()
endif()

if(NOT EXISTS "${GST_HOME_AUDIO_DIR}/libs/meta/dlbaudiometa.h")
  message(FATAL_ERROR
          "Could not locate gst-home-audio metadata headers under ${GST_HOME_AUDIO_DIR}")
endif()

add_library(dlboamdmod SHARED
  src/glib_compat.c
  src/oamd_serializer.cpp
  src/oamdcapsfeature.c
  src/oamdserialize.c
  src/plugin.c
)

target_include_directories(dlboamdmod
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GST_HOME_AUDIO_DIR}/libs/meta"
    ${GST_INCLUDE_DIRS}
)

target_compile_options(dlboamdmod
  PRIVATE
    ${GST_CFLAGS_OTHER}
    -Wall
    -Wextra
)

target_compile_features(dlboamdmod PRIVATE c_std_11 cxx_std_23)

target_link_libraries(dlboamdmod
  PRIVATE
    glaze::glaze
    "${GST_AUDIO_LIBRARY}"
    "${GST_BASE_LIBRARY}"
    "${GSTREAMER_LIBRARY}"
    "${GOBJECT_LIBRARY}"
    "${GLIB_LIBRARY}"
    "${INTL_LIBRARY}"
    "${ICONV_LIBRARY}"
)

set_target_properties(dlboamdmod PROPERTIES
  OUTPUT_NAME "dlboamdmod"
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/x86_64"
  INSTALL_NAME_DIR "@rpath"
  BUILD_WITH_INSTALL_NAME_DIR ON
  MACOSX_RPATH ON
)

add_custom_target(prepare_reference_player_plugins
  COMMAND "${CMAKE_COMMAND}"
          -DREF_PLAYER_PLUGIN_DIR=${REF_PLAYER_PLUGIN_DIR}
          -DCUSTOM_PLUGIN=$<TARGET_FILE:dlboamdmod>
          -DOUT_DIR=${REFERENCE_PLAYER_PLUGINS_DIR}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareReferencePlayerPlugins.cmake"
  DEPENDS dlboamdmod
  VERBATIM
)

install(TARGETS dlboamdmod
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0")

message(STATUS "Reference Player runtime root: ${REF_PLAYER_GST_ROOT}")
message(STATUS "Reference Player plugin directory: ${REF_PLAYER_PLUGIN_DIR}")
message(STATUS "Reference Player scanner: ${REF_PLAYER_GST_SCANNER}")
message(STATUS "Reference Player plugin staging dir: ${REFERENCE_PLAYER_PLUGINS_DIR}")
if(GLAZE_SOURCE_DIR)
  message(STATUS "Using local Glaze source dir: ${GLAZE_SOURCE_DIR}")
else()
  message(STATUS "Glaze will be fetched automatically from GitHub")
endif()
if(GST_HOME_AUDIO_SOURCE_DIR)
  message(STATUS "Using local gst-home-audio source dir: ${GST_HOME_AUDIO_SOURCE_DIR}")
else()
  message(STATUS "gst-home-audio will be populated under: ${GST_HOME_AUDIO_DIR}")
endif()
