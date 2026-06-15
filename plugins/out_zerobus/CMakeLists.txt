set(ZEROBUS_SDK_ROOT "$ENV{HOME}/zerobus-sdk")
set(ZEROBUS_FFI_INCLUDE "${ZEROBUS_SDK_ROOT}/rust/ffi")
set(ZEROBUS_FFI_LIB "${ZEROBUS_SDK_ROOT}/go/lib/linux_amd64/libzerobus_ffi.a")

set(src
    zerobus.c)

FLB_PLUGIN(out_zerobus "${src}" "")

# Add Zerobus SDK include directory
target_include_directories(flb-plugin-out_zerobus PRIVATE
    ${ZEROBUS_FFI_INCLUDE}
)

# Link Zerobus SDK static library and required system libraries.
# These match the link flags the Zerobus Go SDK uses for linux on its cgo
# directive (libzerobus_ffi.a -ldl -lpthread -lm -lresolv -lgcc_s); resolv and
# gcc_s are needed to resolve the static lib's DNS and stack-unwind symbols.
target_link_libraries(flb-plugin-out_zerobus
    ${ZEROBUS_FFI_LIB}
    pthread
    dl
    m
    resolv
    gcc_s
)
