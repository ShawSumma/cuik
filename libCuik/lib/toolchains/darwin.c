#include <cuik.h>

static void add_libraries(void* ctx, const Cuik_CompilerArgs* args, Cuik_Linker* l) {
}

static void set_preprocessor(void* ctx, const Cuik_CompilerArgs* args, Cuik_CPP* cpp) {
    cuikpp_define_cstr(cpp, "__APPLE__", "1");
    cuikpp_define_cstr(cpp, "__MACH__" , "1");
    cuikpp_define_cstr(cpp, "__weak", "// __attribute__((objc_gc(weak))");
    cuikpp_define_cstr(cpp, "__apple_build_version__", "14000029");
}

static bool invoke_link(void* ctx, const Cuik_CompilerArgs* args, Cuik_Linker* linker, const char* filename) {
    return false;
}

Cuik_Toolchain cuik_toolchain_darwin(void) {
    return (Cuik_Toolchain){
        // .ctx = result,
        .set_preprocessor = set_preprocessor,
        .add_libraries = add_libraries,
        .invoke_link = invoke_link
    };
}
