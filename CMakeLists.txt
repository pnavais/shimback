cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SHIMBACK_VERSION LIMIT_COUNT 1)

project(shimback LANGUAGES C VERSION ${SHIMBACK_VERSION})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# With extensions off, -std=c11 is strict ISO C -- glibc (unlike macOS's
# libc, which exposes these regardless) hides POSIX-only declarations
# (strtok_r, clock_gettime/CLOCK_MONOTONIC, ...) behind a feature-test
# macro in that mode. Without it, strtok_r in particular isn't just a
# warning: it's implicitly declared as returning int, silently truncating
# the real pointer it returns on any 64-bit platform. POSIX.1-2008 covers
# everything this project uses.
add_compile_definitions(_POSIX_C_SOURCE=200809L)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(SHIMBACK_DEV_WARNINGS "Build with -Wall -Wextra -Werror" ${CMAKE_BUILD_TYPE_IS_DEBUG})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(SHIMBACK_DEV_WARNINGS ON)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

add_executable(shimback
  src/main.c
  src/cli.c
  src/config.c
  src/dispatch.c
  src/shell.c
  src/paths.c
  src/suggest.c
  src/tui.c
  src/util.c
  src/commands/add.c
  src/commands/add_wizard.c
  src/commands/remove.c
  src/commands/init.c
  src/commands/list.c
  src/commands/doctor.c
  src/commands/install.c
  src/commands/uninstall.c
  src/commands/edit.c
)

target_include_directories(shimback PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(SHIMBACK_DEV_WARNINGS)
  target_compile_options(shimback PRIVATE -Wall -Wextra -Werror)
endif()

install(TARGETS shimback RUNTIME DESTINATION bin)
install(FILES man/shimback.1 DESTINATION share/man/man1)

enable_testing()

add_executable(test_config_parser
  tests/test_config_parser.c
  src/config.c
  src/paths.c
  src/util.c
)
target_include_directories(test_config_parser PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
add_test(NAME test_config_parser COMMAND test_config_parser)

add_test(NAME test_dispatch
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch.sh $<TARGET_FILE:shimback>)
add_test(NAME test_add_remove
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_remove.sh $<TARGET_FILE:shimback>)
add_test(NAME test_init
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_init.sh $<TARGET_FILE:shimback>)
add_test(NAME test_doctor
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_doctor.sh $<TARGET_FILE:shimback>)
add_test(NAME test_install
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.sh $<TARGET_FILE:shimback>)
add_test(NAME test_uninstall
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_uninstall.sh $<TARGET_FILE:shimback>)
add_test(NAME test_add_wizard
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_add_wizard.sh $<TARGET_FILE:shimback>)
add_test(NAME test_list
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_list.sh $<TARGET_FILE:shimback>)
add_test(NAME test_edit
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edit.sh $<TARGET_FILE:shimback>)
add_test(NAME test_split_config
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_split_config.sh $<TARGET_FILE:shimback>)
