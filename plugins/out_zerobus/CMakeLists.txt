# The Zerobus SDK FFI library + header are located by the top-level build
# (pkg-config, with a find_path/find_library prefix fallback — see the "Zerobus
# SDK FFI" block in CMakeLists.txt). This plugin is only added when FLB_OUT_ZEROBUS
# is enabled and the library was found, so ZEROBUS_FFI_INCLUDE_DIR /
# ZEROBUS_FFI_LIB are guaranteed set here.
set(src
    zerobus.c
    zerobus_util.c
    unity_catalog.c)

FLB_PLUGIN(out_zerobus "${src}" "")

target_include_directories(flb-plugin-out_zerobus PRIVATE ${ZEROBUS_FFI_INCLUDE_DIR})

# Link the Zerobus SDK FFI library. The extra system libraries (-ldl -lpthread
# -lm -lresolv -lgcc_s) are required when linking the *static* archive
# (libzerobus_ffi.a, the preferred form) — its Rust code pulls in DNS (resolv)
# and stack-unwind (gcc_s) symbols. They are harmless when the shared library
# is used instead.
target_link_libraries(flb-plugin-out_zerobus
    ${ZEROBUS_FFI_LIB}
    pthread
    dl
    m
    resolv
    gcc_s
)
