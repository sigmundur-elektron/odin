cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project("odin" LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# several pinned dependencies still declare a pre-3.5 minimum, which cmake 4
# rejects outright. this is the documented escape hatch; remove it once every
# dependency below has caught up.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# --- build hardening (off by default, so a plain build stays plain) ---

# a warning that only becomes an error on someone else's machine is not a
# warning, it is an ambush - gcc 14 promoting -Wchanges-meaning is exactly how
# the reproc pin above got discovered. CI turns this on so that toolchain drift
# lands as a deliberate red build instead of as whatever the next compiler
# decides to escalate on its own.
option(ODIN_WERROR "Treat compiler warnings as errors" OFF)

# address + undefined behaviour sanitizers. the highest-value check odin has:
# this is a codebase of raw pipe buffers, file descriptor and HANDLE lifetimes,
# fork/exec and signals, plus a hand-rolled utf-8 decoder - all of it the kind of
# defect that stays silent in debug on two platforms and corrupts a state file on
# a third.
#
# deliberately set before FetchContent so the dependencies are instrumented too.
# reproc is where odin's buffers are actually handed to the operating system, so
# leaving it uninstrumented would blind the sanitizer to the seam that matters.
option(ODIN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ODIN_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "ODIN_SANITIZE is a gcc/clang build; msvc is covered by the plain jobs")
  endif()
  # -fno-sanitize-recover is not optional. without it ubsan prints a diagnostic,
  # carries on, and the process still exits 0 - CI would stay green while
  # standing on undefined behaviour.
  add_compile_options(
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
    -g
  )
  add_link_options(-fsanitize=address,undefined)
endif()

# applied to odin's own targets only. the dependencies are pinned and not ours to
# keep warning-clean, and -Werror on someone else's code is how a routine
# dependency bump turns into an unrelated emergency.
function(odin_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(ODIN_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(ODIN_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

include(FetchContent)

# --- nlohmann/json (durable state files) ---
# chosen for byte-parity with python's json.dump, not for throughput:
# the default std::map backing sorts keys, and dump() exposes ensure_ascii.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)

# --- toml++ (odin.toml, read-only) ---
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE
)

# --- doctest (tests) ---
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE
)

# --- reproc++ (child processes with hard timeouts) ---
# the sidecar and, from P4, every adapter and gate. hand-rolling this is the
# single highest-risk piece of the port: writing stdin while draining stdout
# without deadlocking, and killing a whole process tree on timeout.
set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  reproc
  GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
  # 14.2.8, not v14.2.4: the older header declared `options::env` and
  # `options::redirect` members that shadowed the `reproc::env` class and
  # `reproc::redirect` struct they were spelled in terms of. gcc 14 turned that
  # into a hard error (-Wchanges-meaning, P1787). upstream fixed it by
  # qualifying the names and using elaborated type specifiers.
  # note the tag has no leading "v"; upstream dropped the prefix after 14.2.5.
  GIT_TAG 14.2.8
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json tomlplusplus doctest reproc)

# --- CLI11 (argument parsing) ---
# nested subcommands, repeatable --set, and a required subcommand, matching the
# argparse surface in harness/cli.py.
FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CLI11)

# --- core ---
# a static library rather than a single executable, so the imgui gui can link
# the engine directly instead of shelling out to the binary.
add_library(odin_core STATIC
  src/atomic_file.cpp
  src/json_io.cpp
  src/run_store.cpp
  src/config.cpp
  src/schema_validator.cpp
  src/credentials.cpp
  src/secure_input.cpp
  src/auth_command.cpp
  src/executable.cpp
  src/http_client.cpp
  src/discovery.cpp
  src/output_extract.cpp
  src/prompt_builder.cpp
  src/doctor_command.cpp
  src/tools_command.cpp
  src/contracts.cpp
  src/definitions.cpp
  src/subprocess.cpp
  src/adapter.cpp
  src/engine.cpp
)
target_include_directories(odin_core PUBLIC src)
target_link_libraries(odin_core PUBLIC
  nlohmann_json::nlohmann_json
  tomlplusplus::tomlplusplus
  reproc++
)

# --- HTTP transport ---
# TLS is delegated to the platform rather than bundled: WinHTTP uses the system
# certificate and proxy stores, so there is no CA file to ship or keep current,
# and libcurl is the equivalent everywhere else. Odin implements no TLS itself.
if(WIN32)
  target_link_libraries(odin_core PUBLIC winhttp ws2_32)
else()
  find_package(CURL REQUIRED)
  target_link_libraries(odin_core PUBLIC CURL::libcurl)
endif()
# toml++ defaults to throwing on a parse error. odin reports failure through a
# trailing out_error instead, and this switches toml++ to the matching mode
# where parse() returns a result carrying .error() rather than throwing.
target_compile_definitions(odin_core PUBLIC TOML_EXCEPTIONS=0)

if(MSVC)
  # the on-disk format is utf-8 end to end; msvc needs telling twice.
  # /EHsc is not in cmake 4's default msvc flags, and while odin's own code
  # reports failure through out_error rather than throwing, nlohmann, toml++ and
  # doctest all require exceptions to be enabled.
  target_compile_options(odin_core PUBLIC /utf-8 /EHsc)
endif()
odin_set_warnings(odin_core)

# --- cli ---
set(ODIN_RUNTIME_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/odin")
cmake_path(
  RELATIVE_PATH ODIN_RUNTIME_INSTALL_DIR
  BASE_DIRECTORY "${CMAKE_INSTALL_BINDIR}"
  OUTPUT_VARIABLE ODIN_RUNTIME_RELATIVE
)

add_executable(odin src/main.cpp src/cli.cpp src/runtime_paths.cpp)
target_link_libraries(odin PRIVATE odin_core CLI11::CLI11)
target_compile_definitions(odin PRIVATE
  ODIN_RUNTIME_RELATIVE="${ODIN_RUNTIME_RELATIVE}"
  ODIN_SOURCE_RUNTIME_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)
odin_set_warnings(odin)

install(TARGETS odin RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY harness/
  DESTINATION "${ODIN_RUNTIME_INSTALL_DIR}/harness"
  FILES_MATCHING PATTERN "*.json"
)

# --- tests ---

# a real child process for the tests that exercise the process boundary.
# it used to be `python -c "<snippet>"`, which made an interpreter a hard
# requirement for ctest long after the product stopped needing one.
add_executable(odin_test_child tests/cpp/test_child.cpp)
target_link_libraries(odin_test_child PRIVATE nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(odin_test_child PRIVATE /utf-8 /EHsc)
endif()
odin_set_warnings(odin_test_child)

add_executable(odin_tests
  tests/cpp/test_main.cpp
  tests/cpp/test_json_io.cpp
  tests/cpp/test_run_store.cpp
  tests/cpp/test_config.cpp
  tests/cpp/test_schema_validator.cpp
  tests/cpp/test_contracts.cpp
  tests/cpp/test_credentials.cpp
  tests/cpp/test_tools.cpp
  tests/cpp/test_output_extract.cpp
  tests/cpp/test_builtin_adapters.cpp
  tests/cpp/test_subprocess.cpp
  tests/cpp/test_utf8.cpp
  tests/cpp/test_engine.cpp
)
target_link_libraries(odin_tests PRIVATE odin_core doctest::doctest)
target_compile_definitions(odin_tests PRIVATE
  ODIN_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
  ODIN_TEST_CHILD="$<TARGET_FILE:odin_test_child>")
add_dependencies(odin_tests odin_test_child)
odin_set_warnings(odin_tests)

enable_testing()
add_test(NAME odin_tests COMMAND odin_tests)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME odin_install_smoke
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_installed.py"
    --cmake "${CMAKE_COMMAND}"
    --build-dir "${CMAKE_BINARY_DIR}"
    --config "$<CONFIG>"
    --bindir "${CMAKE_INSTALL_BINDIR}"
    --runtime-dir "${ODIN_RUNTIME_INSTALL_DIR}"
    --helper "$<TARGET_FILE:odin_test_child>"
)
set_tests_properties(odin_install_smoke PROPERTIES
  LABELS "integration;install"
  TIMEOUT 180
)

add_test(
  NAME odin_interruption_smoke
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_interruption.py"
    --odin "$<TARGET_FILE:odin>"
    --runtime-root "${CMAKE_CURRENT_SOURCE_DIR}"
    --helper "$<TARGET_FILE:odin_test_child>"
)
set_tests_properties(odin_interruption_smoke PROPERTIES
  LABELS "integration;interruption"
  TIMEOUT 120
)
