cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )

  # QtKeychain stores API credentials in the OS-native credential store
  # (macOS Keychain / Windows Credential Manager / Linux Secret Service) instead
  # of plaintext config.json. It is not part of the obs-deps Qt6 bundle, so vendor
  # it from source and link it statically into the plugin module.
  include(FetchContent)
  set(BUILD_WITH_QT6 ON CACHE INTERNAL "")
  set(BUILD_TEST_APPLICATION OFF CACHE INTERNAL "")
  set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
  # obs-deps Qt6 ships no LinguistTools, and we don't need QtKeychain's UI strings.
  set(BUILD_TRANSLATIONS OFF CACHE INTERNAL "")
  # Skip QtKeychain's own autotest (needs Qt6Test, absent from obs-deps Qt6).
  set(BUILD_TESTING OFF CACHE INTERNAL "")
  FetchContent_Declare(
    qt6keychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG 0.16.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(qt6keychain)
  # The project compiles with -Werror -Wall (compilerconfig); don't hold the
  # vendored dependency to our warning policy. QtKeychain's keychain_apple.mm is
  # written for manual reference counting, so disable the project-wide ObjC ARC
  # default for this target only.
  if(TARGET qt6keychain)
    target_compile_options(qt6keychain PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w>)
    set_target_properties(qt6keychain PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC NO)
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE qt6keychain)
  target_include_directories(
    ${CMAKE_PROJECT_NAME}
    PRIVATE ${qt6keychain_SOURCE_DIR}/qtkeychain ${qt6keychain_BINARY_DIR}/qtkeychain
  )
endif()

find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

find_package(Threads REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/subsplash-api.c src/scheduler.c src/scheduler-panel.cpp src/credential-store.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(WIN32)
  configure_file(installer.iss.in installer.iss)
endif()

option(ENABLE_TESTS "Build unit tests" OFF)
if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
