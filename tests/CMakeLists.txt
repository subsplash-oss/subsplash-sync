cmake_minimum_required(VERSION 3.14)
project(subsplash-sync-tests C)

include(FetchContent)

FetchContent_Declare(
  cmocka
  GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
  GIT_TAG cmocka-1.1.7
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18
  GIT_SHALLOW TRUE
)

set(WITH_STATIC_LIB ON CACHE BOOL "" FORCE)
set(WITH_CMOCKERY_SUPPORT OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)

find_package(Threads REQUIRED)
enable_testing()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

FetchContent_MakeAvailable(cmocka cjson)

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

FetchContent_GetProperties(cjson SOURCE_DIR CJSON_SOURCE_DIR)

# -- test-scheduler ------------------------------------------------

add_executable(test-scheduler
  test-scheduler.c
  stubs/obs-stubs.c
)

target_include_directories(test-scheduler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${SRC_DIR}
  ${CJSON_SOURCE_DIR}
)

target_link_libraries(test-scheduler PRIVATE
  cmocka::cmocka
  cjson
  Threads::Threads
)

add_test(NAME test-scheduler COMMAND test-scheduler)

# -- test-subsplash-api --------------------------------------------

add_executable(test-subsplash-api
  test-subsplash-api.c
  stubs/obs-stubs.c
  stubs/curl-stubs.c
)

target_include_directories(test-subsplash-api PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${SRC_DIR}
  ${CJSON_SOURCE_DIR}
)

target_link_libraries(test-subsplash-api PRIVATE
  cmocka::cmocka
  cjson
  Threads::Threads
)

add_test(NAME test-subsplash-api COMMAND test-subsplash-api)
