#include <cuik.h>
#include "helper.h"
#include "spall_perf.h"
#include <dyn_array.h>

#ifndef __CUIK__
#define CUIK_ALLOW_THREADS 1
#else
#define CUIK_ALLOW_THREADS 0
#endif

#if CUIK_ALLOW_THREADS
#include <threads.h>
#include <stdatomic.h>
#include "threadpool.h"
#endif

#include "live.h"

static void exit_or_hook(int code) {
    if (IsDebuggerPresent()) {
        __debugbreak();
    }
    exit(code);
}

/*static void initialize_opt_passes(void) {
    da_passes = dyn_array_create(TB_Pass, 32);

    if (args.opt_level) {
        dyn_array_put(da_passes, tb_opt_hoist_locals());
        dyn_array_put(da_passes, tb_opt_merge_rets());

        dyn_array_put(da_passes, tb_opt_instcombine());
        dyn_array_put(da_passes, tb_opt_dead_expr_elim());
        dyn_array_put(da_passes, tb_opt_dead_block_elim());
        dyn_array_put(da_passes, tb_opt_subexpr_elim());

        dyn_array_put(da_passes, tb_opt_mem2reg());
        dyn_array_put(da_passes, tb_opt_remove_pass_nodes());
        dyn_array_put(da_passes, tb_opt_instcombine());
        dyn_array_put(da_passes, tb_opt_remove_pass_nodes());
        dyn_array_put(da_passes, tb_opt_dead_expr_elim());
        dyn_array_put(da_passes, tb_opt_dead_block_elim());
        dyn_array_put(da_passes, tb_opt_subexpr_elim());
        dyn_array_put(da_passes, tb_opt_remove_pass_nodes());
        dyn_array_put(da_passes, tb_opt_compact_dead_regs());

        // dyn_array_put(da_passes, tb_opt_inline());

        // aggresive optimizations
        // TODO(NeGate): loop optimizations, data structure reordering
        // switch optimizations

        // dyn_array_put(da_passes, tb_opt_remove_pass_nodes());
        // dyn_array_put(da_passes, tb_opt_compact_dead_regs());
    }
}*/

#if CUIK_ALLOW_THREADS
static int calculate_worker_thread_count(void) {
    #ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (sysinfo.dwNumberOfProcessors / 4) * 3;
    #else
    return 1;
    #endif
}

static void tp_submit(void* user_data, void fn(void*), void* arg) {
    threadpool_submit((threadpool_t*) user_data, fn, arg);
}

static void tp_work_one_job(void* user_data) {
    threadpool_work_one_job((threadpool_t*) user_data);
}
#endif

static void dump_tokens(FILE* out_file, TokenStream* s) {
    const char* last_file = NULL;
    int last_line = 0;

    Token* tokens = cuikpp_get_tokens(s);
    size_t count = cuikpp_get_token_count(s);

    for (size_t i = 0; i < count; i++) {
        Token* t = &tokens[i];

        ResolvedSourceLoc r = cuikpp_find_location(s, t->location);
        if (last_file != r.file->filename) {
            // TODO(NeGate): Kinda shitty but i just wanna duplicate
            // the backslashes to avoid them being treated as an escape
            const char* in = (const char*) r.file->filename;
            char str[FILENAME_MAX], *out = str;

            while (*in) {
                if (*in == '\\') {
                    *out++ = '\\';
                    *out++ = '\\';
                    in++;
                } else {
                    *out++ = *in++;
                }
            }
            *out++ = '\0';

            fprintf(out_file, "\n#line %d \"%s\"\t", r.line, str);
            last_file = r.file->filename;
        }

        if (last_line != r.line) {
            fprintf(out_file, "\n/* line %3d */\t", r.line);
            last_line = r.line;
        }

        fprintf(out_file, "%.*s ", (int) t->content.length, t->content.data);
    }
}

int main(int argc, const char** argv) {
    cuik_init();
    find_system_deps();

    Cuik_CompilerArgs args = {
        .version = CUIK_VERSION_C23,
        .target = cuik_host_target(),
        .flavor = TB_FLAVOR_EXECUTABLE,
        .crt_dirpath = crt_dirpath,
    };
    cuik_parse_args(&args, argc - 1, argv + 1);

    if (dyn_array_length(args.sources) == 0) {
        fprintf(stderr, "error: no input files!\n");
        return EXIT_FAILURE;
    }

    if (args.time) {
        char output_path_no_ext[FILENAME_MAX];
        cuik_driver_get_output_path(&args, FILENAME_MAX, output_path_no_ext);

        #if 0
        char* perf_output_path = cuikperf_init(FILENAME_MAX, &json_profiler, false);
        sprintf_s(perf_output_path, FILENAME_MAX, "%s.json", output_path_no_ext);
        #else
        char* perf_output_path = cuikperf_init(FILENAME_MAX, &spall_profiler, false);
        sprintf_s(perf_output_path, FILENAME_MAX, "%s.spall", output_path_no_ext);
        #endif

        cuikperf_start();
    }

    Cuik_IThreadpool* ithread_pool = NULL;

    // spin up worker threads
    #if CUIK_ALLOW_THREADS
    threadpool_t* thread_pool = NULL;
    if (args.threads > 1) {
        if (args.verbose) printf("Starting with %d threads...\n", args.threads);

        thread_pool = threadpool_create(args.threads - 1, 4096);
        ithread_pool = malloc(sizeof(Cuik_IThreadpool));
        *ithread_pool = (Cuik_IThreadpool){
            .user_data = thread_pool,
            .submit = tp_submit,
            .work_one_job = tp_work_one_job
        };
    }
    #endif

    if (args.preprocess) {
        // preproc only
        Cuik_CPP* cpp = cuik_driver_preprocess(args.sources[0], &args, true);
        if (cpp) {
            dump_tokens(stdout, cuikpp_get_token_stream(cpp));
            cuikpp_free(cpp);
        } else {
            fprintf(stderr, "Could not preprocess file: %s", args.sources[0]);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    if (args.live) {
        LiveCompiler l;
        do {
            printf("\x1b[2J");
            printf("OUTPUT OF %s:\n", args.sources[0]);

            cuik_driver_compile(ithread_pool, &args, true);
        } while (live_compile_watch(&l, &args));
    } else {
        uint64_t start_time = args.verbose ? cuik_time_in_nanos() : 0;
        int status = cuik_driver_compile(ithread_pool, &args, true);

        if (args.verbose) {
            uint64_t now = cuik_time_in_nanos();
            printf("\n\nCUIK: %f ms\n", (now - start_time) / 1000000.0);
        }

        if (status != 0) exit_or_hook(status);
    }

    #if CUIK_ALLOW_THREADS
    if (thread_pool != NULL) {
        threadpool_free(thread_pool);
        thread_pool = NULL;
    }
    #endif

    if (args.time) cuikperf_stop();
    return 0;
}