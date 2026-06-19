# ---------------------------------------------------------------------------
# Zerobus SDK source
#
# The SDK (including its C FFI) is pulled from a pinned Git commit that exposes
# the dynamic protobuf-schema functions (zerobus_proto_schema_*). CMake fetches
# that exact commit and builds the FFI static library from source with cargo,
# because the prebuilt libzerobus_ffi.a committed in the repo does not (yet)
# contain those new symbols. Building from source guarantees the linked library
# matches the commit's headers.
#
# Override the commit/repo (or point at a local checkout) with -D flags:
#   -DZEROBUS_SDK_GIT_URL=... -DZEROBUS_SDK_GIT_REF=<sha|branch|tag>
#   -DZEROBUS_SDK_DIR=/path/to/existing/checkout   (skips the fetch)
# ---------------------------------------------------------------------------
set(ZEROBUS_SDK_GIT_URL "https://github.com/databricks/zerobus-sdk.git"
    CACHE STRING "Zerobus SDK git repository")
# Pinned to an exact commit for reproducible builds: the head of the dynamic
# protobuf-schema C FFI branch (databricks/zerobus-sdk#371). Bump this SHA to
# pick up SDK changes; after #371 merges, repoint at the merge commit on main.
set(ZEROBUS_SDK_GIT_REF "a5b7efa280950949c513a49dacbdab239d45947a"
    CACHE STRING "Zerobus SDK git commit (SHA, branch, or tag)")
set(ZEROBUS_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/zerobus-sdk"
    CACHE PATH "Zerobus SDK checkout location")

set(ZEROBUS_FFI_INCLUDE "${ZEROBUS_SDK_DIR}/rust/ffi")
set(ZEROBUS_FFI_LIB "${ZEROBUS_SDK_DIR}/rust/target/release/libzerobus_ffi.a")

find_program(CARGO_EXECUTABLE cargo
             HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
            "cargo not found; the out_zerobus plugin needs the Rust toolchain "
            "to build the Zerobus SDK FFI from source")
endif()

# Fetch the pinned SDK commit at configure time (once) so the FFI header path
# resolves. `git clone --branch` only accepts branch/tag names, so to pin an
# exact SHA we init + shallow-fetch that commit + checkout it (GitHub allows
# fetching an arbitrary commit by hash).
if(NOT EXISTS "${ZEROBUS_SDK_DIR}/rust/ffi/zerobus.h")
    message(STATUS "out_zerobus: fetching Zerobus SDK ${ZEROBUS_SDK_GIT_REF} from ${ZEROBUS_SDK_GIT_URL}")
    execute_process(
        COMMAND git init -q ${ZEROBUS_SDK_DIR}
        RESULT_VARIABLE _zb_result)
    if(_zb_result EQUAL 0)
        execute_process(
            COMMAND git -C ${ZEROBUS_SDK_DIR} remote add origin ${ZEROBUS_SDK_GIT_URL}
            RESULT_VARIABLE _zb_result)
    endif()
    if(_zb_result EQUAL 0)
        execute_process(
            COMMAND git -C ${ZEROBUS_SDK_DIR} fetch --depth 1 origin ${ZEROBUS_SDK_GIT_REF}
            RESULT_VARIABLE _zb_result)
    endif()
    if(_zb_result EQUAL 0)
        execute_process(
            COMMAND git -C ${ZEROBUS_SDK_DIR} checkout -q FETCH_HEAD
            RESULT_VARIABLE _zb_result)
    endif()
    if(NOT _zb_result EQUAL 0)
        message(FATAL_ERROR "out_zerobus: failed to fetch Zerobus SDK commit ${ZEROBUS_SDK_GIT_REF}")
    endif()
endif()

# This box (and CI) cannot reach crates.io directly; route the SDK build through
# the Databricks crate proxy (same mirror as universe/third_party/rust/config.toml).
file(WRITE "${ZEROBUS_SDK_DIR}/rust/.cargo/config.toml"
"net.git-fetch-with-cli = true
net.retry = 5
[http]
timeout = 120
[source.crates-io]
replace-with = \"databricks-proxy\"
[source.databricks-proxy]
registry = \"sparse+https://crates-proxy.cloud.databricks.com/\"
")

# Build the FFI static library from the cloned source (contains the
# zerobus_proto_schema_* symbols the plugin calls).
add_custom_command(
    OUTPUT "${ZEROBUS_FFI_LIB}"
    COMMAND ${CARGO_EXECUTABLE} build --release -p zerobus-ffi
    WORKING_DIRECTORY "${ZEROBUS_SDK_DIR}/rust"
    COMMENT "Building Zerobus SDK FFI from ${ZEROBUS_SDK_GIT_REF} (cargo)"
    VERBATIM)
add_custom_target(zerobus_ffi_build DEPENDS "${ZEROBUS_FFI_LIB}")

set(src
    zerobus.c
    zerobus_util.c
    unity_catalog.c)

FLB_PLUGIN(out_zerobus "${src}" "")

# Make sure the SDK FFI library is built (and the header is present) before the
# plugin compiles/links.
add_dependencies(flb-plugin-out_zerobus zerobus_ffi_build)

target_include_directories(flb-plugin-out_zerobus PRIVATE
    ${ZEROBUS_FFI_INCLUDE}
)

# Link the Zerobus SDK FFI static library and the system libraries its Rust code
# needs (-ldl -lpthread -lm -lresolv -lgcc_s): resolv and gcc_s resolve the
# static lib's DNS and stack-unwind symbols respectively.
target_link_libraries(flb-plugin-out_zerobus
    ${ZEROBUS_FFI_LIB}
    pthread
    dl
    m
    resolv
    gcc_s
)
