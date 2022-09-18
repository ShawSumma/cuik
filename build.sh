#! /bin/sh -e
mkdir -p "bin"
mkdir -p "tilde-backend/bin"
cd "tilde-backend"
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/debug_builder.c -g -c -o bin/debug_builder.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/exporter.c -g -c -o bin/exporter.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/hash.c -g -c -o bin/hash.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/ir_printer.c -g -c -o bin/ir_printer.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/iter.c -g -c -o bin/iter.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb.c -g -c -o bin/tb.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_analysis.c -g -c -o bin/tb_analysis.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_atomic.c -g -c -o bin/tb_atomic.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_builder.c -g -c -o bin/tb_builder.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_internal.c -g -c -o bin/tb_internal.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_jit.c -g -c -o bin/tb_jit.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/tb_optimizer.c -g -c -o bin/tb_optimizer.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/validator.c -g -c -o bin/validator.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/codegen/tree.c -g -c -o bin/tree.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/bigint/BigInt.c -g -c -o bin/BigInt.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/objects/coff.c -g -c -o bin/coff.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/objects/coff_parse.c -g -c -o bin/coff_parse.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/objects/elf64.c -g -c -o bin/elf64.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/objects/macho.c -g -c -o bin/macho.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/objects/pe.c -g -c -o bin/pe.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/debug/codeview.c -g -c -o bin/codeview.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/debug/cv/cv_type_builder.c -g -c -o bin/cv_type_builder.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/x64/x64.c -g -c -o bin/x64.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/x64/x64_new.c -g -c -o bin/x64_new.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/aarch64/aarch64.c -g -c -o bin/aarch64.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/branchless.c -g -c -o bin/branchless.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/canonical.c -g -c -o bin/canonical.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/copy_elision.c -g -c -o bin/copy_elision.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/dead_code_elim.c -g -c -o bin/dead_code_elim.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/deshort_circuit.c -g -c -o bin/deshort_circuit.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/hoist_locals.c -g -c -o bin/hoist_locals.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/load_elim.c -g -c -o bin/load_elim.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/mem2reg.c -g -c -o bin/mem2reg.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/merge_ret.c -g -c -o bin/merge_ret.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/opt/refinement.c -g -c -o bin/refinement.o
clang -I include -I deps/luajit/src -Wall -Werror -Wno-unused-function -DTB_COMPILE_TESTS src/tb/system/posix.c -g -c -o bin/posix.o
cd deps/luajit/src && make CC=clang BUILDMODE=static && cd ../../..
mkdir -p deps/luajit/src/bin && cd deps/luajit/src/bin && ar -x ../libluajit.a && cd ../../../..
ar -rcs tildebackend.a bin/*.o deps/luajit/src/bin/*.o
cd ".."
clang lexgen.c -o bin/a.out && bin/a.out lib/preproc/dfa.h
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/arena.c -g -c -o bin/arena.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/compilation_unit.c -g -c -o bin/compilation_unit.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/crash_handler.c -g -c -o bin/crash_handler.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/cuik.c -g -c -o bin/cuik.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/diagnostic.c -g -c -o bin/diagnostic.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/file_cache.c -g -c -o bin/file_cache.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/str.c -g -c -o bin/str.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/timer.c -g -c -o bin/timer.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/tls.c -g -c -o bin/tls.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/preproc/cpp.c -g -c -o bin/cpp.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/preproc/lexer.c -g -c -o bin/lexer.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/ast_dump.c -g -c -o bin/ast_dump.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/ast_optimizer.c -g -c -o bin/ast_optimizer.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/atoms.c -g -c -o bin/atoms.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/parser.c -g -c -o bin/parser.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/sema.c -g -c -o bin/sema.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/types.c -g -c -o bin/types.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/front/visitors.c -g -c -o bin/visitors.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/targets/target_generic.c -g -c -o bin/target_generic.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/targets/x64_desc.c -g -c -o bin/x64_desc.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/back/ir_gen.c -g -c -o bin/ir_gen.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include lib/back/linker.c -g -c -o bin/linker.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include deps/mimalloc/src/static.c -g -c -o bin/static.o
ar -rcs bin/libcuik.a bin/arena.o bin/ast_dump.o bin/ast_optimizer.o bin/atoms.o bin/compilation_unit.o bin/cpp.o bin/crash_handler.o bin/cuik.o bin/diagnostic.o bin/file_cache.o bin/ir_gen.o bin/lexer.o bin/linker.o bin/parser.o bin/sema.o bin/static.o bin/str.o bin/target_generic.o bin/timer.o bin/tls.o bin/types.o bin/visitors.o bin/x64_desc.o
clang -DCUIK_USE_TB -I include -I lib -I deps -I tilde-backend/include -Wall -Werror -Wno-unused-function -Wno-unused-variable -DCUIK_USE_TB -DTB_COMPILE_TESTS -msse4.2 -maes -DMI_MALLOC_OVERRIDE -I deps/mimalloc/include drivers/main_driver.c drivers/threadpool.c drivers/bindgen_c99.c drivers/bindgen_odin.c bin/libcuik.a tilde-backend/tildebackend.a -lm -ldl -lpthread -g -o bin/cuik
