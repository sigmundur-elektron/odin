cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project("odin" LANGUAGES CXX)

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
  GIT_TAG v14.2.4
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json tomlplusplus doctest reproc)

# CLI11 (P6, argument parsing) is added when its phase lands.

# --- core ---
# a static library rather than a single executable, so the imgui gui can link
# the engine directly instead of shelling out to the binary.
add_library(odin_core STATIC
  src/atomic_file.cpp
  src/json_io.cpp
  src/config.cpp
  src/sidecar.cpp
  src/contracts.cpp
  src/definitions.cpp
  src/process.cpp
  src/adapter.cpp
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
add_executable(odin src/main.cpp)
target_link_libraries(odin PRIVATE odin_core)

# --- tests ---
add_executable(odin_tests
  tests/cpp/test_main.cpp
  tests/cpp/test_json_io.cpp
  tests/cpp/test_json_parity.cpp
  tests/cpp/test_config.cpp
  tests/cpp/test_contracts.cpp
  tests/cpp/test_process.cpp
)
target_link_libraries(odin_tests PRIVATE odin_core doctest::doctest)
target_compile_definitions(odin_tests PRIVATE
  ODIN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  ODIN_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME odin_tests COMMAND odin_tests)
