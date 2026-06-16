set(ZEROBUS_SDK_ROOT "$ENV{HOME}/zerobus-sdk")
set(ZEROBUS_FFI_INCLUDE "${ZEROBUS_SDK_ROOT}/rust/ffi")
set(ZEROBUS_FFI_LIB "${ZEROBUS_SDK_ROOT}/go/lib/linux_amd64/libzerobus_ffi.a")

# ---------------------------------------------------------------------------
# Companion Rust FFI crate (zerobus_uc_ffi)
#
# Bridges the two operations the prebuilt C FFI does not expose: building a
# protobuf descriptor from a Unity Catalog schema, and encoding a record into
# protobuf bytes. It links the Zerobus SDK as a *library* (a path dependency, the
# same way the Vector sink consumes it) and is built as a cdylib so its private
# Rust runtime does not collide with the prebuilt libzerobus_ffi.a static lib.
#
# NOTE: building this crate fetches its dependencies from crates.io (or your
# configured registry mirror), so the build host needs network/registry access.
# ---------------------------------------------------------------------------
set(UC_FFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zerobus_uc_ffi")
set(UC_FFI_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/zerobus_uc_ffi_target")
set(UC_FFI_LIB "${UC_FFI_TARGET_DIR}/release/libzerobus_uc_ffi.so")

find_program(CARGO_EXECUTABLE cargo
             HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
            "cargo not found; the out_zerobus plugin needs the Rust toolchain "
            "to build the zerobus_uc_ffi companion crate")
endif()

# Generate Cargo.toml from the template, substituting the SDK path dependency.
configure_file("${UC_FFI_DIR}/Cargo.toml.in" "${UC_FFI_DIR}/Cargo.toml" @ONLY)

add_custom_command(
    OUTPUT "${UC_FFI_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${UC_FFI_TARGET_DIR}"
            ${CARGO_EXECUTABLE} build --release
            --manifest-path "${UC_FFI_DIR}/Cargo.toml"
    DEPENDS "${UC_FFI_DIR}/src/lib.rs" "${UC_FFI_DIR}/Cargo.toml.in"
    WORKING_DIRECTORY "${UC_FFI_DIR}"
    COMMENT "Building zerobus_uc_ffi companion crate (cargo)"
    VERBATIM
)
add_custom_target(zerobus_uc_ffi_build DEPENDS "${UC_FFI_LIB}")

set(src
    zerobus.c
    unity_catalog.c)

FLB_PLUGIN(out_zerobus "${src}" "")

# The plugin links the companion crate; make sure it is built first.
add_dependencies(flb-plugin-out_zerobus zerobus_uc_ffi_build)

# Add Zerobus SDK include directory
target_include_directories(flb-plugin-out_zerobus PRIVATE
    ${ZEROBUS_FFI_INCLUDE}
)

# Link Zerobus SDK static library, the companion crate, and required system
# libraries. The system libs match the link flags the Zerobus Go SDK uses for
# linux on its cgo directive (libzerobus_ffi.a -ldl -lpthread -lm -lresolv
# -lgcc_s); resolv and gcc_s resolve the static lib's DNS and stack-unwind
# symbols. Linking ${UC_FFI_LIB} by its full path in the build tree makes CMake
# add its directory to the final binary's RPATH automatically.
target_link_libraries(flb-plugin-out_zerobus
    ${ZEROBUS_FFI_LIB}
    ${UC_FFI_LIB}
    pthread
    dl
    m
    resolv
    gcc_s
)

# libzerobus_uc_ffi.so lives in the build tree. Propagate an RPATH (as an
# INTERFACE option, so it reaches the final fluent-bit binary that links this
# plugin) pointing at its directory. If you relocate the binary, set
# LD_LIBRARY_PATH to this directory instead.
target_link_options(flb-plugin-out_zerobus INTERFACE
    "-Wl,-rpath,${UC_FFI_TARGET_DIR}/release"
)
