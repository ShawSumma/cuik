#pragma once
#include <back/tb.h>
#include <stdatomic.h>

typedef struct CompilerSettings {
	const char* output_path;
	TB_OptLevel optimization_level;
	
	bool is_object_only : 1;
	bool print_tb_ir : 1;
	bool debug_info : 1;
	bool pedantic : 1;
	
	atomic_bool using_winmain;
	int num_of_worker_threads;
} CompilerSettings;

extern TB_Arch target_arch;
extern TB_System target_sys;
extern CompilerSettings settings;

