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
  src/sidecar.cpp
  src/contracts.cpp
  src/definitions.cpp
  src/subprocess.cpp
  src/adapter.cpp
  src/engine.cpp
  src/delegate.cpp
)
target_include_directories(odin_core PUBLIC src)
target_link_libraries(odin_core PUBLIC
  nlohmann_json::nlohmann_json
  tomlplusplus::tomlplusplus
  reproc++
)

# toml++ defaults to throwing on a parse error. odin reports failure through a
# trailing out_error instead, and this switches toml++ to the matching mode
# where parse() returns a result carrying .error() rather than throwing.
target_compile_definitions(odin_core PUBLIC TOML_EXCEPTIONS=0)

if(MSVC)
  target_compile_options(odin_core PRIVATE /W4 /permissive-)
  # the on-disk format is utf-8 end to end; msvc needs telling twice.
  # /EHsc is not in cmake 4's default msvc flags, and while odin's own code
  # reports failure through out_error rather than throwing, nlohmann, toml++ and
  # doctest all require exceptions to be enabled.
  target_compile_options(odin_core PUBLIC /utf-8 /EHsc)
else()
  target_compile_options(odin_core PRIVATE -Wall -Wextra)
endif()

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

install(TARGETS odin RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES odin.py DESTINATION "${ODIN_RUNTIME_INSTALL_DIR}")
install(DIRECTORY harness/
  DESTINATION "${ODIN_RUNTIME_INSTALL_DIR}/harness"
  FILES_MATCHING PATTERN "*.py" PATTERN "*.json"
)
install(FILES scripts/contract_service.py scripts/requirements.txt
  DESTINATION "${ODIN_RUNTIME_INSTALL_DIR}/scripts"
)
install(FILES adapters/cli_agent.py adapters/openai_compatible.py
  DESTINATION "${ODIN_RUNTIME_INSTALL_DIR}/adapters"
)

# --- tests ---
add_executable(odin_tests
  tests/cpp/test_main.cpp
  tests/cpp/test_json_io.cpp
  tests/cpp/test_json_parity.cpp
  tests/cpp/test_run_store.cpp
  tests/cpp/test_config.cpp
  tests/cpp/test_contracts.cpp
  tests/cpp/test_subprocess.cpp
  tests/cpp/test_engine.cpp
  tests/cpp/test_engine_parity.cpp
)
target_link_libraries(odin_tests PRIVATE odin_core doctest::doctest)
target_compile_definitions(odin_tests PRIVATE
  ODIN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  ODIN_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

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
    --python "${Python3_EXECUTABLE}"
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
    --python "${Python3_EXECUTABLE}"
)
set_tests_properties(odin_interruption_smoke PROPERTIES
  LABELS "integration;interruption"
  TIMEOUT 120
)
