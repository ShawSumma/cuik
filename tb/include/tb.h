#ifndef TB_CORE_H
#define TB_CORE_H

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// https://semver.org/
#define TB_VERSION_MAJOR 0
#define TB_VERSION_MINOR 2
#define TB_VERSION_PATCH 0

#define TB_API extern

#ifdef __cplusplus
extern "C" {
    #endif

    // These are flags
    typedef enum TB_ArithmaticBehavior {
        TB_ARITHMATIC_NSW = 1,
        TB_ARITHMATIC_NUW = 2,
    } TB_ArithmaticBehavior;

    typedef enum TB_DebugFormat {
        TB_DEBUGFMT_NONE,

        TB_DEBUGFMT_DWARF,
        TB_DEBUGFMT_CODEVIEW,

        TB_DEBUGFMT_COLINPILLED
    } TB_DebugFormat;

    typedef enum TB_Arch {
        TB_ARCH_UNKNOWN,

        TB_ARCH_X86_64,
        TB_ARCH_AARCH64, // unsupported but planned
        TB_ARCH_WASM32,
    } TB_Arch;

    typedef enum TB_System {
        TB_SYSTEM_WINDOWS,
        TB_SYSTEM_LINUX,
        TB_SYSTEM_MACOS,
        TB_SYSTEM_ANDROID, // Not supported yet
        TB_SYSTEM_WEB,

        TB_SYSTEM_MAX,
    } TB_System;

    typedef enum TB_ABI {
        // Used on 64bit Windows platforms
        TB_ABI_WIN64,

        // Used on Mac, BSD and Linux platforms
        TB_ABI_SYSTEMV,
    } TB_ABI;

    typedef enum TB_OutputFlavor {
        TB_FLAVOR_OBJECT,     // .o  .obj
        TB_FLAVOR_ASSEMBLY,   // .s  .asm
        TB_FLAVOR_SHARED,     // .so .dll
        TB_FLAVOR_STATIC,     // .a  .lib
        TB_FLAVOR_EXECUTABLE, //     .exe
    } TB_OutputFlavor;

    typedef enum TB_CallingConv {
        TB_CDECL,
        TB_STDCALL
    } TB_CallingConv;

    typedef enum TB_FeatureSet_X64 {
        TB_FEATURE_X64_SSE3   = (1u << 0u),
        TB_FEATURE_X64_SSE41  = (1u << 1u),
        TB_FEATURE_X64_SSE42  = (1u << 2u),

        TB_FEATURE_X64_POPCNT = (1u << 3u),
        TB_FEATURE_X64_LZCNT  = (1u << 4u),

        TB_FEATURE_X64_CLMUL  = (1u << 5u),
        TB_FEATURE_X64_F16C   = (1u << 6u),

        TB_FEATURE_X64_BMI1   = (1u << 7u),
        TB_FEATURE_X64_BMI2   = (1u << 8u),

        TB_FEATURE_X64_AVX    = (1u << 9u),
        TB_FEATURE_X64_AVX2   = (1u << 10u),
    } TB_FeatureSet_X64;

    typedef struct TB_FeatureSet {
        TB_FeatureSet_X64 x64;
    } TB_FeatureSet;

    typedef enum TB_BranchHint {
        TB_BRANCH_HINT_NONE,
        TB_BRANCH_HINT_LIKELY,
        TB_BRANCH_HINT_UNLIKELY
    } TB_BranchHint;

    typedef enum TB_Linkage {
        TB_LINKAGE_PUBLIC,
        TB_LINKAGE_PRIVATE
    } TB_Linkage;

    typedef enum {
        TB_COMDAT_NONE,

        TB_COMDAT_MATCH_ANY,
    } TB_ComdatType;

    typedef enum TB_MemoryOrder {
        TB_MEM_ORDER_RELAXED,
        TB_MEM_ORDER_CONSUME,
        TB_MEM_ORDER_ACQUIRE,
        TB_MEM_ORDER_RELEASE,
        TB_MEM_ORDER_ACQ_REL,
        TB_MEM_ORDER_SEQ_CST,
    } TB_MemoryOrder;

    typedef enum TB_ISelMode {
        // FastISel
        TB_ISEL_FAST,
        TB_ISEL_COMPLEX
    } TB_ISelMode;

    typedef enum TB_DataTypeEnum {
        // Integers, note void is an i0 and bool is an i1
        //   i(0-2047)
        TB_INT,
        // Floating point numbers
        //   f{32,64}
        TB_FLOAT,
        // Pointers
        //   ptr(0-2047)
        TB_PTR,
    } TB_DataTypeEnum;

    typedef enum TB_FloatFormat {
        // IEEE 754 floats
        TB_FLT_32, TB_FLT_64
    } TB_FloatFormat;

    typedef union TB_DataType {
        struct {
            uint8_t type;
            // 2^N where N is the width value.
            // Only integers and floats can be wide.
            uint8_t width;
            // for integers it's the bitwidth
            uint16_t data;
        };
        uint32_t raw;
    } TB_DataType;

    // classify data types
    #define TB_IS_VOID_TYPE(x)     ((x).type == TB_INT && (x).data == 0)
    #define TB_IS_BOOL_TYPE(x)     ((x).type == TB_INT && (x).data == 1)
    #define TB_IS_INTEGER_TYPE(x)  ((x).type == TB_INT)
    #define TB_IS_FLOAT_TYPE(x)    ((x).type == TB_FLOAT)
    #define TB_IS_POINTER_TYPE(x)  ((x).type == TB_PTR)

    // accessors
    #define TB_GET_INT_BITWIDTH(x) ((x).data)
    #define TB_GET_FLOAT_FORMAT(x) ((x).data)
    #define TB_GET_PTR_ADDRSPACE(x) ((x).data)

    typedef enum TB_NodeTypeEnum {
        TB_NULL = 0,

        /* metadata */
        TB_LINE_INFO,
        TB_KEEPALIVE,

        TB_CALL,  /* normal call */
        TB_SCALL, /* system call */

        /* Memory operations */
        TB_STORE,

        TB_MEMCPY,
        TB_MEMSET,

        /* Atomics */
        TB_ATOMIC_TEST_AND_SET,
        TB_ATOMIC_CLEAR,

        TB_ATOMIC_LOAD,
        TB_ATOMIC_XCHG,
        TB_ATOMIC_ADD,
        TB_ATOMIC_SUB,
        TB_ATOMIC_AND,
        TB_ATOMIC_XOR,
        TB_ATOMIC_OR,

        TB_ATOMIC_CMPXCHG, /* These are always bundled together */
        TB_ATOMIC_CMPXCHG2,
        TB_DEBUGBREAK,

        /* Terminators */
        TB_BRANCH,
        TB_RET,
        TB_UNREACHABLE,
        TB_TRAP,

        TB_POISON,

        /* Load */
        TB_LOAD,

        /* Pointers */
        TB_LOCAL,

        TB_PARAM,
        TB_GET_SYMBOL_ADDRESS,

        TB_MEMBER_ACCESS,
        TB_ARRAY_ACCESS,

        /* Immediates */
        TB_INTEGER_CONST,
        TB_FLOAT32_CONST,
        TB_FLOAT64_CONST,

        /* Conversions */
        TB_TRUNCATE,
        TB_FLOAT_EXT,
        TB_SIGN_EXT,
        TB_ZERO_EXT,
        TB_INT2PTR,
        TB_PTR2INT,
        TB_UINT2FLOAT,
        TB_FLOAT2UINT,
        TB_INT2FLOAT,
        TB_FLOAT2INT,
        TB_BITCAST,

        /* Select */
        TB_SELECT,

        /* Bitmagic */
        TB_BSWAP,
        TB_CLZ,
        TB_CTZ,
        TB_POPCNT,

        /* Unary operations */
        TB_NOT,
        TB_NEG,

        /* Integer arithmatic */
        TB_AND,
        TB_OR,
        TB_XOR,
        TB_ADD,
        TB_SUB,
        TB_MUL,

        TB_SHL,
        TB_SHR,
        TB_SAR,
        TB_UDIV,
        TB_SDIV,
        TB_UMOD,
        TB_SMOD,

        /* Float arithmatic */
        TB_FADD,
        TB_FSUB,
        TB_FMUL,
        TB_FDIV,

        /* Comparisons */
        TB_CMP_EQ,
        TB_CMP_NE,
        TB_CMP_ULT,
        TB_CMP_ULE,
        TB_CMP_SLT,
        TB_CMP_SLE,
        TB_CMP_FLT,
        TB_CMP_FLE,

        // PHI
        TB_PHI,

        // NOTE(NeGate): only used internally, if you
        // see one in normal IR things went wrong in
        // an optimization pass
        TB_PASS,

        // variadic
        TB_VA_START,

        // x86 intrinsics
        TB_X86INTRIN_RDTSC,
        TB_X86INTRIN_LDMXCSR,
        TB_X86INTRIN_STMXCSR,
        TB_X86INTRIN_SQRT,
        TB_X86INTRIN_RSQRT,
    } TB_NodeTypeEnum;
    typedef uint8_t TB_NodeType;

    #define TB_IS_NODE_SIDE_EFFECT(type) ((type) >= TB_LINE_INFO && (type) <= TB_DEBUGBREAK)
    #define TB_IS_NODE_TERMINATOR(type)  ((type) >= TB_BRANCH && (type) <= TB_TRAP)

    typedef int TB_Label;

    typedef struct {
        int64_t  key;
        TB_Label value;
    } TB_SwitchEntry;

    // just represents some region of bytes, usually in file parsing crap
    typedef struct {
        size_t length;
        const uint8_t* data;
    } TB_Slice;

    // represents byte counts
    typedef uint32_t TB_CharUnits;

    typedef unsigned int TB_AttributeID;
    typedef unsigned int TB_FileID;

    // SO refers to shared objects which mean either shared libraries (.so or .dll)
    // or executables (.exe or ELF executables)
    typedef enum {
        // exports to the rest of the shared object
        TB_EXTERNAL_SO_LOCAL,

        // exports outside of the shared object
        TB_EXTERNAL_SO_EXPORT,
    } TB_ExternalType;

    typedef struct TB_Global            TB_Global;
    typedef struct TB_External          TB_External;
    typedef struct TB_Function          TB_Function;

    typedef struct TB_Module            TB_Module;
    typedef struct TB_Attrib            TB_Attrib;
    typedef struct TB_DebugType         TB_DebugType;
    typedef struct TB_ModuleSection     TB_ModuleSection;
    typedef struct TB_FunctionPrototype TB_FunctionPrototype;

    // Refers generically to objects within a module
    //
    // TB_Function, TB_Global, and TB_External are all subtypes of TB_Symbol
    // and thus are safely allowed to cast into a symbol for operations.
    typedef struct TB_Symbol {
        enum TB_SymbolTag {
            TB_SYMBOL_NONE,

            // symbol is dead now
            TB_SYMBOL_TOMBSTONE,

            TB_SYMBOL_EXTERNAL,
            TB_SYMBOL_GLOBAL,
            TB_SYMBOL_FUNCTION,

            TB_SYMBOL_MAX,
        } tag;

        // refers to the next symbol with the same tag
        struct TB_Symbol* next;
        char* name;

        // It's kinda a weird circular reference but yea
        TB_Module* module;

        // helpful for sorting and getting consistent builds
        int ordinal;

        union {
            // if we're JITing then this maps to the address of the symbol
            void* address;
            size_t symbol_id;
        };

        // after this point it's tag-specific storage
    } TB_Symbol;

    typedef int TB_Reg;

    #define TB_NULL_REG NULL

    typedef struct TB_Node TB_Node;
    struct TB_Node {
        TB_NodeType type;
        TB_DataType dt;
        uint16_t input_count; // number of node inputs
        uint16_t extra_count; // number of bytes for extra operand data

        TB_Node*    next;
        TB_Attrib*  first_attrib;

        // input list
        TB_Node* inputs[/* input_count */];
    };

    // These are the extra data in specific nodes
    #define TB_NODE_GET_EXTRA(n)         ((void*) &n->inputs[n->input_count])
    #define TB_NODE_GET_EXTRA_T(n, T)    ((T*) &n->inputs[n->input_count])
    #define TB_NODE_SET_EXTRA(n, T, ...) (*((T*) &n->inputs[n->input_count]) = (T){ __VA_ARGS__ })

    // this represents switch (many targets), if (one target) and goto (only default) logic.
    typedef struct { // TB_BRANCH
        uint32_t count;
        TB_Label default_label;
        TB_SwitchEntry targets[/* input_count */];
    } TB_NodeBranch;

    typedef struct { // TB_PARAM
        uint32_t id;
        TB_CharUnits size;
    } TB_NodeParam;

    typedef struct { // TB_INT
        uint64_t num_words;
        uint64_t words[];
    } TB_NodeInt;

    typedef struct { // any compare operator
        TB_DataType cmp_dt;
    } TB_NodeCompare;

    typedef struct { // any integer binary operator
        TB_ArithmaticBehavior ab;
    } TB_NodeBinopInt;

    typedef struct {
        TB_CharUnits align;
        bool is_volatile;
    } TB_NodeMemAccess;

    typedef struct {
        TB_CharUnits size, align;
    } TB_NodeLocal;

    typedef struct {
        TB_FileID file;
        int line;
    } TB_NodeLine;

    typedef struct {
        float value;
    } TB_NodeFloat32;

    typedef struct {
        double value;
    } TB_NodeFloat64;

    typedef struct {
        int64_t stride;
    } TB_NodeArray;

    typedef struct {
        int64_t offset;
    } TB_NodeMember;

    typedef struct {
        const TB_Symbol* sym;
    } TB_NodeSymbol;

    typedef struct {
        TB_MemoryOrder order;
        TB_MemoryOrder order2;
    } TB_NodeAtomic;

    typedef struct {
        TB_Label labels[];
    } TB_NodePhi;

    typedef struct {
        TB_Label label;
        TB_Node* val;
    } TB_PhiInput;

    typedef struct TB_BasicBlock {
        TB_Node *start, *end;
    } TB_BasicBlock;

    #define TB_KILL_NODE(n) ((n)->type = TB_NULL)

    typedef struct TB_Loop {
        // refers to another entry in TB_LoopInfo... unless it's -1
        ptrdiff_t parent_loop;

        TB_Label header;
        TB_Label backedge;

        size_t body_count;
        TB_Label* body;
    } TB_Loop;

    typedef struct TB_LoopInfo {
        size_t count;
        TB_Loop* loops;
    } TB_LoopInfo;

    typedef struct TB_Predeccesors {
        int* count;
        TB_Label** preds;
    } TB_Predeccesors;

    typedef struct TB_DominanceFrontiers TB_DominanceFrontiers;

    typedef enum {
        TB_OBJECT_RELOC_NONE, // how?

        // Target independent
        TB_OBJECT_RELOC_ADDR32,
        TB_OBJECT_RELOC_ADDR64, // unsupported on 32bit platforms
        TB_OBJECT_RELOC_SECREL,
        TB_OBJECT_RELOC_SECTION,

        // COFF only
        TB_OBJECT_RELOC_ADDR32NB, // Relative virtual address

        // x64 only
        TB_OBJECT_RELOC_REL32,    // relative 32bit displacement

        // Aarch64 only
        TB_OBJECT_RELOC_BRANCH26, // 26bit displacement for B and BL instructions
        TB_OBJECT_RELOC_REL21,    // for ADR instructions

        // TODO(NeGate): fill in the rest of this later
    } TB_ObjectRelocType;

    typedef struct {
        TB_ObjectRelocType type;
        uint32_t symbol_index;
        size_t virtual_address;
        size_t addend;
    } TB_ObjectReloc;

    typedef enum {
        TB_OBJECT_SYMBOL_UNKNOWN,
        TB_OBJECT_SYMBOL_EXTERN,      // exported
        TB_OBJECT_SYMBOL_WEAK_EXTERN, // weak
        TB_OBJECT_SYMBOL_IMPORT,      // forward decl
        TB_OBJECT_SYMBOL_STATIC,      // local
        TB_OBJECT_SYMBOL_SECTION,     // local
    } TB_ObjectSymbolType;

    typedef struct {
        TB_ObjectSymbolType type;
        int section_num;

        uint32_t ordinal;
        uint32_t value;

        TB_Slice name;

        // for COFF, this is the auxillary
        void* extra;

        // this is zeroed out by the loader and left for the user to do crap with
        void* user_data;
    } TB_ObjectSymbol;

    typedef enum {
        TB_COFF_SECTION_NO_PAD      = 0x00000008,
        TB_COFF_SECTION_CODE        = 0x00000020,
        TB_COFF_SECTION_INIT        = 0x00000040,
        TB_COFF_SECTION_UNINIT      = 0x00000080,
        TB_COFF_SECTION_OTHER       = 0x00000100,
        TB_COFF_SECTION_INFO        = 0x00000200,
        TB_COFF_SECTION_REMOVE      = 0x00000800,
        TB_COFF_SECTION_COMDAT      = 0x00001000,

        // this is actually a 4bit field
        TB_COFF_SECTION_ALIGN       = 0x00F00000,

        // if we have more than 65535 relocations we do this
        TB_COFF_SECTION_RELOC_OVR   = 0x00F00000,

        // memory flags
        TB_COFF_SECTION_DISCARDABLE = 0x02000000,
        TB_COFF_SECTION_NOT_CACHED  = 0x04000000,
        TB_COFF_SECTION_NOT_PAGED   = 0x08000000,
        TB_COFF_SECTION_SHARED      = 0x10000000,
        TB_COFF_SECTION_EXECUTE     = 0x20000000,
        TB_COFF_SECTION_READ        = 0x40000000,
        TB_COFF_SECTION_WRITE       = 0x80000000,
    } TB_COFF_SectionFlags;

    typedef struct {
        TB_Slice name;
        uint32_t flags;

        // the section symbol... if applies
        TB_ObjectSymbol* sym;

        size_t virtual_address;
        size_t virtual_size;

        // You can have a virtual size without having a raw
        // data size, that's how the BSS section works
        TB_Slice raw_data;

        size_t relocation_count;
        TB_ObjectReloc* relocations;

        // this is zeroed out by the loader and left for the user to do crap with
        void* user_data;
    } TB_ObjectSection;

    typedef enum {
        TB_OBJECT_FILE_UNKNOWN,

        TB_OBJECT_FILE_COFF,
        TB_OBJECT_FILE_ELF64
    } TB_ObjectFileType;

    typedef struct {
        TB_ObjectFileType type;
        TB_Arch           arch;

        TB_Slice          name;
        TB_Slice          ar_name;

        size_t           symbol_count;
        TB_ObjectSymbol* symbols;

        size_t           section_count;
        TB_ObjectSection sections[];
    } TB_ObjectFile;

    typedef struct {
        TB_Slice name;

        // if import_name is empty, we're dealing with an object file
        TB_Slice import_name;
        uint16_t ordinal;

        TB_ObjectFile* obj;
    } TB_ArchiveEntry;

    typedef struct {
        TB_Slice file;
        size_t pos;

        size_t member_count;
        uint32_t* members;

        size_t symbol_count;
        uint16_t* symbols;

        TB_Slice strtbl;
    } TB_ArchiveFileParser;

    typedef enum {
        TB_EXECUTABLE_UNKNOWN,
        TB_EXECUTABLE_PE,
        TB_EXECUTABLE_ELF,
    } TB_ExecutableType;

    // *******************************
    // Public macros
    // *******************************
    #ifdef __cplusplus

    #define TB_TYPE_VOID    TB_DataType{ { TB_INT,   0, 0 } }
    #define TB_TYPE_I8      TB_DataType{ { TB_INT,   0, 8 } }
    #define TB_TYPE_I16     TB_DataType{ { TB_INT,   0, 16 } }
    #define TB_TYPE_I32     TB_DataType{ { TB_INT,   0, 32 } }
    #define TB_TYPE_I64     TB_DataType{ { TB_INT,   0, 64 } }
    #define TB_TYPE_F32     TB_DataType{ { TB_FLOAT, 0, TB_FLT_32 } }
    #define TB_TYPE_F64     TB_DataType{ { TB_FLOAT, 0, TB_FLT_64 } }
    #define TB_TYPE_BOOL    TB_DataType{ { TB_INT,   0, 1 } }
    #define TB_TYPE_PTR     TB_DataType{ { TB_PTR,   0, 0 } }

    #define TB_TYPE_INTN(N) TB_DataType{ { TB_INT,   0, (N) } }
    #define TB_TYPE_PTRN(N) TB_DataType{ { TB_PTR,   0, (N) } }

    #else

    #define TB_TYPE_VOID (TB_DataType){ { TB_INT,   0, 0 } }
    #define TB_TYPE_I8   (TB_DataType){ { TB_INT,   0, 8 } }
    #define TB_TYPE_I16  (TB_DataType){ { TB_INT,   0, 16 } }
    #define TB_TYPE_I32  (TB_DataType){ { TB_INT,   0, 32 } }
    #define TB_TYPE_I64  (TB_DataType){ { TB_INT,   0, 64 } }
    #define TB_TYPE_F32  (TB_DataType){ { TB_FLOAT, 0, TB_FLT_32 } }
    #define TB_TYPE_F64  (TB_DataType){ { TB_FLOAT, 0, TB_FLT_64 } }
    #define TB_TYPE_BOOL (TB_DataType){ { TB_INT,   0, 1 } }
    #define TB_TYPE_PTR  (TB_DataType){ { TB_PTR,   0, 0 } }
    #define TB_TYPE_INTN(N) (TB_DataType){ { TB_INT,  0, (N) } }
    #define TB_TYPE_PTRN(N) (TB_DataType){ { TB_PTR,  0, (N) } }

    #endif

    typedef void (*TB_PrintCallback)(void* user_data, const char* fmt, ...);

    ////////////////////////////////
    // Module management
    ////////////////////////////////
    // Creates a module with the correct target and settings
    TB_API TB_Module* tb_module_create(TB_Arch arch, TB_System sys, const TB_FeatureSet* features, bool is_jit);

    // Creates a module but defaults on the architecture and system based on the host machine
    TB_API TB_Module* tb_module_create_for_host(const TB_FeatureSet* features, bool is_jit);

    // compiles the function into machine code. For isel_mode, TB_ISEL_FAST
    // will compile faster but worse codegen
    // TB_ISEL_COMPLEX will compile slower but better codegen
    //
    // returns false if it fails.
    TB_API bool tb_module_compile_function(TB_Module* m, TB_Function* f, TB_ISelMode isel_mode);

    TB_API size_t tb_module_get_function_count(TB_Module* m);

    // Frees all resources for the TB_Module and it's functions, globals and
    // compiled code.
    TB_API void tb_module_destroy(TB_Module* m);

    // When targetting windows & thread local storage, you'll need to bind a tls index
    // which is usually just a global that the runtime support has initialized, if you
    // dont and the tls_index is used, it'll crash
    TB_API void tb_module_set_tls_index(TB_Module* m, TB_Symbol* e);

    // You don't need to manually call this unless you want to resolve locations before
    // exporting.
    TB_API void tb_module_layout_sections(TB_Module* m);

    ////////////////////////////////
    // Exporter
    ////////////////////////////////
    // some terminology, in this context virtual file means it's not necessarily tie to the
    // normal file system but refers to a buffer of memory that's potentially going to either
    // go into the normal file system or some other persistent space.

    // the maximum number of "virtual files" any single export call can produce
    // it's two because EXE export with debug info produces a PDB as well
    enum { TB_MAX_EXPORTS = 2 };

    typedef struct {
        size_t count;
        struct {
            size_t length;
            uint8_t* data;
        } files[TB_MAX_EXPORTS];
    } TB_Exports;

    TB_API TB_Exports tb_module_object_export(TB_Module* m, TB_DebugFormat debug_fmt);
    TB_API void tb_exporter_free(TB_Exports exports);

    TB_API TB_Exports tb_module_export_bytecode(TB_Module* m);

    ////////////////////////////////
    // Linker exporter
    ////////////////////////////////
    // This is used to export shared objects or executables
    typedef struct TB_Linker TB_Linker;
    typedef struct TB_LinkerSection TB_LinkerSection;
    typedef struct TB_LinkerSectionPiece TB_LinkerSectionPiece;

    TB_API TB_ExecutableType tb_system_executable_format(TB_System s);

    TB_API TB_Linker* tb_linker_create(TB_ExecutableType type, TB_Arch arch);
    TB_API TB_Exports tb_linker_export(TB_Linker* l);
    TB_API void tb_linker_destroy(TB_Linker* l);

    // Links compiled module into output
    TB_API void tb_linker_append_module(TB_Linker* l, TB_Module* m);

    // Adds static library to output
    //   this can include imports (wrappers for DLL symbols) along with
    //   normal sections.
    TB_API void tb_linker_append_library(TB_Linker* l, TB_Slice ar_name, TB_Slice ar_file);

    ////////////////////////////////
    // JIT compilation
    ////////////////////////////////
    typedef struct TB_JITContext TB_JITContext;

    // passing 0 to jit_heap_capacity will default to 4MiB
    TB_API TB_JITContext* tb_module_begin_jit(TB_Module* m, size_t jit_heap_capacity);
    TB_API void tb_module_end_jit(TB_JITContext* jit);

    #define TB_FOR_FUNCTIONS(it, module) for (TB_Function* it = tb_first_function(module); it != NULL; it = tb_next_function(it))
    TB_API TB_Function* tb_first_function(TB_Module* m);
    TB_API TB_Function* tb_next_function(TB_Function* f);

    #define TB_FOR_EXTERNALS(it, module) for (TB_External* it = tb_first_external(module); it != NULL; it = tb_next_external(it))
    TB_API TB_External* tb_first_external(TB_Module* m);
    TB_API TB_External* tb_next_external(TB_External* e);

    // this is used JIT scenarios to tell the compiler what externals map to
    TB_API TB_ExternalType tb_extern_get_type(TB_External* e);

    TB_API TB_External* tb_extern_create(TB_Module* m, const char* name, TB_ExternalType type);
    TB_API TB_FileID tb_file_create(TB_Module* m, const char* path);

    // Called once you're done with TB operations on a thread (or i guess when it's
    // about to be killed :p), not calling it can only result in leaks on that thread
    // and calling it too early will result in TB potentially reallocating it but there's
    // should be no crashes from this, just potential slowdown or higher than expected memory
    // usage.
    TB_API void tb_free_thread_resources(void);

    ////////////////////////////////
    // Function Prototypes
    ////////////////////////////////
    // creates a function prototype used to define a function's parameters and
    // return type.
    //
    // function prototypes do not get freed individually and last for the entire run
    // of the backend, they can also be reused for multiple functions which have
    // matching signatures.
    TB_API TB_FunctionPrototype* tb_prototype_create(TB_Module* m, TB_CallingConv conv, TB_DataType return_dt, TB_DebugType* return_type, int num_params, bool has_varargs);

    // adds a parameter to the function prototype, TB doesn't support struct
    // parameters so the frontend must lower them to pointers or any other type
    // depending on their preferred ABI.
    TB_API void tb_prototype_add_param(TB_Module* m, TB_FunctionPrototype* p, TB_DataType dt);
    TB_API void tb_prototype_add_param_named(TB_Module* m, TB_FunctionPrototype* p, TB_DataType dt, const char* name, TB_DebugType* debug_type);

    ////////////////////////////////
    // Globals
    ////////////////////////////////
    TB_API TB_Global* tb_global_create(TB_Module* m, const char* name, TB_DebugType* dbg_type, TB_Linkage linkage);

    // allocate space for the global
    TB_API void tb_global_set_storage(TB_Module* m, TB_ModuleSection* section, TB_Global* global, size_t size, size_t align, size_t max_objects);

    // returns a buffer which the user can fill to then have represented in the initializer
    TB_API void* tb_global_add_region(TB_Module* m, TB_Global* global, size_t offset, size_t size);

    // places a relocation for a global at offset, the size of the relocation
    // depends on the pointer size
    TB_API void tb_global_add_symbol_reloc(TB_Module* m, TB_Global* global, size_t offset, const TB_Symbol* symbol);

    TB_API TB_ModuleSection* tb_module_get_text(TB_Module* m);
    TB_API TB_ModuleSection* tb_module_get_rdata(TB_Module* m);
    TB_API TB_ModuleSection* tb_module_get_data(TB_Module* m);
    TB_API TB_ModuleSection* tb_module_get_tls(TB_Module* m);

    ////////////////////////////////
    // Function Attributes
    ////////////////////////////////
    // These are parts of a function that describe metadata for instructions
    TB_API void tb_function_attrib_variable(TB_Function* f, TB_Node* n, const char* name, TB_DebugType* type);

    ////////////////////////////////
    // Debug info Generation
    ////////////////////////////////
    TB_API TB_DebugType* tb_debug_get_void(TB_Module* m);
    TB_API TB_DebugType* tb_debug_get_bool(TB_Module* m);
    TB_API TB_DebugType* tb_debug_get_integer(TB_Module* m, bool is_signed, int bits);
    TB_API TB_DebugType* tb_debug_get_float(TB_Module* m, TB_FloatFormat fmt);
    TB_API TB_DebugType* tb_debug_create_ptr(TB_Module* m, TB_DebugType* base);
    TB_API TB_DebugType* tb_debug_create_array(TB_Module* m, TB_DebugType* base, size_t count);
    TB_API TB_DebugType* tb_debug_create_struct(TB_Module* m, const char* tag);
    TB_API TB_DebugType* tb_debug_create_union(TB_Module* m, const char* tag);
    TB_API TB_DebugType* tb_debug_create_field(TB_Module* m, TB_DebugType* type, const char* name, TB_CharUnits offset);
    TB_API void tb_debug_complete_record(TB_DebugType* type, TB_DebugType** members, size_t count, TB_CharUnits size, TB_CharUnits align);

    ////////////////////////////////
    // IR access
    ////////////////////////////////
    // #define TB_FOR_BASIC_BLOCK(it, f) for (TB_BBIter it = { 0 }; tb_next_bb(f, &it);)

    // it is an index to the input
    #define TB_FOR_INPUT_IN_NODE(it, parent) for (TB_Node **it = parent->inputs, **__end = it + (parent)->input_count; it != __end; it++)

    ////////////////////////////////
    // Symbols
    ////////////////////////////////
    TB_API bool tb_symbol_is_comdat(const TB_Symbol* s);

    // returns NULL if the tag doesn't match
    TB_API TB_Function* tb_symbol_as_function(TB_Symbol* s);
    TB_API TB_External* tb_symbol_as_external(TB_Symbol* s);
    TB_API TB_Global* tb_symbol_as_global(TB_Symbol* s);

    // used when sorting things within sections, helps make consistent builds
    TB_API void tb_symbol_set_ordinal(TB_Symbol* s, int ordinal);

    ////////////////////////////////
    // Function IR Generation
    ////////////////////////////////
    // the user_data is expected to be a valid FILE*
    TB_API void tb_default_print_callback(void* user_data, const char* fmt, ...);

    // this only allows for power of two vector types
    TB_API TB_DataType tb_vector_type(TB_DataTypeEnum type, int width);

    // if section is NULL, default to .text
    TB_API TB_Function* tb_function_create(TB_Module* m, const char* name, TB_Linkage linkage, TB_ComdatType comdat);

    TB_API void* tb_function_get_jit_pos(TB_Function* f);

    TB_API void tb_symbol_bind_ptr(TB_Symbol* s, void* ptr);
    TB_API void tb_symbol_set_name(TB_Symbol* s, const char* name);
    TB_API const char* tb_symbol_get_name(TB_Symbol* s);

    TB_API void tb_function_set_prototype(TB_Function* f, const TB_FunctionPrototype* p);
    TB_API const TB_FunctionPrototype* tb_function_get_prototype(TB_Function* f);

    TB_API TB_Label tb_basic_block_create(TB_Function* f);
    TB_API bool tb_basic_block_is_complete(TB_Function* f, TB_Label bb);

    TB_API TB_Label tb_inst_get_label(TB_Function* f);
    TB_API void tb_inst_set_label(TB_Function* f, TB_Label l);

    TB_API void tb_function_print(TB_Function* f, TB_PrintCallback callback, void* user_data, bool display_nops);
    TB_API void tb_function_free(TB_Function* f);

    TB_API void tb_inst_loc(TB_Function* f, TB_FileID file, int line);

    TB_API void tb_inst_unreachable(TB_Function* f);
    TB_API void tb_inst_debugbreak(TB_Function* f);
    TB_API void tb_inst_trap(TB_Function* f);
    TB_API void tb_inst_keep_alive(TB_Function* f, TB_Node* src);
    TB_API TB_Node* tb_inst_poison(TB_Function* f);

    TB_API TB_Node* tb_inst_param(TB_Function* f, int param_id);
    TB_API TB_Node* tb_inst_param_addr(TB_Function* f, int param_id);

    TB_API TB_Node* tb_inst_fpxt(TB_Function* f, TB_Node* src, TB_DataType dt);
    TB_API TB_Node* tb_inst_sxt(TB_Function* f, TB_Node* src, TB_DataType dt);
    TB_API TB_Node* tb_inst_zxt(TB_Function* f, TB_Node* src, TB_DataType dt);
    TB_API TB_Node* tb_inst_trunc(TB_Function* f, TB_Node* src, TB_DataType dt);
    TB_API TB_Node* tb_inst_int2ptr(TB_Function* f, TB_Node* src);
    TB_API TB_Node* tb_inst_ptr2int(TB_Function* f, TB_Node* src, TB_DataType dt);
    TB_API TB_Node* tb_inst_int2float(TB_Function* f, TB_Node* src, TB_DataType dt, bool is_signed);
    TB_API TB_Node* tb_inst_float2int(TB_Function* f, TB_Node* src, TB_DataType dt, bool is_signed);
    TB_API TB_Node* tb_inst_bitcast(TB_Function* f, TB_Node* src, TB_DataType dt);

    TB_API TB_Node* tb_inst_local(TB_Function* f, uint32_t size, TB_CharUnits align);
    TB_API TB_Node* tb_inst_load(TB_Function* f, TB_DataType dt, TB_Node* addr, TB_CharUnits align, bool is_volatile);
    TB_API void tb_inst_store(TB_Function* f, TB_DataType dt, TB_Node* addr, TB_Node* val, TB_CharUnits align, bool is_volatile);

    TB_API TB_Node* tb_inst_bool(TB_Function* f, bool imm);
    TB_API TB_Node* tb_inst_ptr(TB_Function* f, uint64_t imm);
    TB_API TB_Node* tb_inst_sint(TB_Function* f, TB_DataType dt, int64_t imm);
    TB_API TB_Node* tb_inst_uint(TB_Function* f, TB_DataType dt, uint64_t imm);
    TB_API TB_Node* tb_inst_float32(TB_Function* f, float imm);
    TB_API TB_Node* tb_inst_float64(TB_Function* f, double imm);
    TB_API TB_Node* tb_inst_cstring(TB_Function* f, const char* str);
    TB_API TB_Node* tb_inst_string(TB_Function* f, size_t len, const char* str);

    // Broadcasts 'val' across 'count' elements starting 'dst'
    TB_API void tb_inst_memset(TB_Function* f, TB_Node* dst, TB_Node* val, TB_Node* count, TB_CharUnits align, bool is_volatile);

    // performs a copy of 'count' elements from one memory location to another
    // both locations cannot overlap.
    TB_API void tb_inst_memcpy(TB_Function* f, TB_Node* dst, TB_Node* src, TB_Node* count, TB_CharUnits align, bool is_volatile);

    // result = base + (index * stride)
    TB_API TB_Node* tb_inst_array_access(TB_Function* f, TB_Node* base, TB_Node* index, int64_t stride);

    // result = base + offset
    // where base is a pointer
    TB_API TB_Node* tb_inst_member_access(TB_Function* f, TB_Node* base, int64_t offset);

    TB_API TB_Node* tb_inst_get_symbol_address(TB_Function* f, const TB_Symbol* target);

    // Performs a conditional select between two values, if the operation is
    // performed wide then the cond is expected to be the same type as a and b where
    // the condition is resolved as true if the MSB (per component) is 1.
    //
    // result = cond ? a : b
    // a, b must match in type
    TB_API TB_Node* tb_inst_select(TB_Function* f, TB_Node* cond, TB_Node* a, TB_Node* b);

    // Integer arithmatic
    TB_API TB_Node* tb_inst_add(TB_Function* f, TB_Node* a, TB_Node* b, TB_ArithmaticBehavior arith_behavior);
    TB_API TB_Node* tb_inst_sub(TB_Function* f, TB_Node* a, TB_Node* b, TB_ArithmaticBehavior arith_behavior);
    TB_API TB_Node* tb_inst_mul(TB_Function* f, TB_Node* a, TB_Node* b, TB_ArithmaticBehavior arith_behavior);
    TB_API TB_Node* tb_inst_div(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);
    TB_API TB_Node* tb_inst_mod(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);

    // Bitmagic operations
    TB_API TB_Node* tb_inst_bswap(TB_Function* f, TB_Node* n);
    TB_API TB_Node* tb_inst_clz(TB_Function* f, TB_Node* n);
    TB_API TB_Node* tb_inst_ctz(TB_Function* f, TB_Node* n);
    TB_API TB_Node* tb_inst_popcount(TB_Function* f, TB_Node* n);

    // Bitwise operations
    TB_API TB_Node* tb_inst_not(TB_Function* f, TB_Node* n);
    TB_API TB_Node* tb_inst_neg(TB_Function* f, TB_Node* n);
    TB_API TB_Node* tb_inst_and(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_or(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_xor(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_sar(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_shl(TB_Function* f, TB_Node* a, TB_Node* b, TB_ArithmaticBehavior arith_behavior);
    TB_API TB_Node* tb_inst_shr(TB_Function* f, TB_Node* a, TB_Node* b);

    // Atomics
    // By default you can use TB_MEM_ORDER_SEQ_CST for the memory order to get
    // correct but possibly slower results on certain platforms (those with relaxed
    // memory models).
    TB_API TB_Node* tb_inst_atomic_test_and_set(TB_Function* f, TB_Node* addr, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_clear(TB_Function* f, TB_Node* addr, TB_MemoryOrder order);

    // Must be aligned to the natural alignment of dt
    TB_API TB_Node* tb_inst_atomic_load(TB_Function* f, TB_Node* addr, TB_DataType dt, TB_MemoryOrder order);

    // All atomic operations here return the old value and the operations are
    // performed in the same data type as 'src' with alignment of 'addr' being
    // the natural alignment of 'src'
    TB_API TB_Node* tb_inst_atomic_xchg(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_add(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_sub(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_and(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_xor(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);
    TB_API TB_Node* tb_inst_atomic_or(TB_Function* f, TB_Node* addr, TB_Node* src, TB_MemoryOrder order);

    // returns old_value from *addr
    TB_API TB_Node* tb_inst_atomic_cmpxchg(TB_Function* f, TB_Node* addr, TB_Node* expected, TB_Node* desired, TB_MemoryOrder succ, TB_MemoryOrder fail);

    // Float math
    TB_API TB_Node* tb_inst_fadd(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_fsub(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_fmul(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_fdiv(TB_Function* f, TB_Node* a, TB_Node* b);

    // Comparisons
    TB_API TB_Node* tb_inst_cmp_eq(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_cmp_ne(TB_Function* f, TB_Node* a, TB_Node* b);

    TB_API TB_Node* tb_inst_cmp_ilt(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);
    TB_API TB_Node* tb_inst_cmp_ile(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);
    TB_API TB_Node* tb_inst_cmp_igt(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);
    TB_API TB_Node* tb_inst_cmp_ige(TB_Function* f, TB_Node* a, TB_Node* b, bool signedness);

    TB_API TB_Node* tb_inst_cmp_flt(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_cmp_fle(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_cmp_fgt(TB_Function* f, TB_Node* a, TB_Node* b);
    TB_API TB_Node* tb_inst_cmp_fge(TB_Function* f, TB_Node* a, TB_Node* b);

    // General intrinsics
    TB_API TB_Node* tb_inst_va_start(TB_Function* f, TB_Node* a);

    // x86 Intrinsics
    TB_API TB_Node* tb_inst_x86_rdtsc(TB_Function* f);
    TB_API TB_Node* tb_inst_x86_ldmxcsr(TB_Function* f, TB_Node* a);
    TB_API TB_Node* tb_inst_x86_stmxcsr(TB_Function* f);
    TB_API TB_Node* tb_inst_x86_sqrt(TB_Function* f, TB_Node* a);
    TB_API TB_Node* tb_inst_x86_rsqrt(TB_Function* f, TB_Node* a);

    // Control flow
    TB_API TB_Node* tb_inst_syscall(TB_Function* f, TB_DataType dt, TB_Node* syscall_num, size_t param_count, TB_Node** params);
    TB_API TB_Node* tb_inst_call(TB_Function* f, TB_DataType dt, TB_Node* target, size_t param_count, TB_Node** params);

    TB_API TB_Node* tb_inst_phi2(TB_Function* f, TB_Label a_label, TB_Node* a, TB_Label b_label, TB_Node* b);
    TB_API void tb_inst_goto(TB_Function* f, TB_Label id);
    TB_API void tb_inst_if(TB_Function* f, TB_Node* cond, TB_Label if_true, TB_Label if_false);
    TB_API void tb_inst_branch(TB_Function* f, TB_DataType dt, TB_Node* key, TB_Label default_label, size_t entry_count, const TB_SwitchEntry* entries);

    TB_API void tb_inst_ret(TB_Function* f, TB_Node* value);

    ////////////////////////////////
    // Optimizer
    ////////////////////////////////
    typedef struct TB_Pass TB_Pass;

    // Applies optimizations to the entire module
    TB_API void tb_module_optimize(TB_Module* m, size_t pass_count, const TB_Pass* passes[]);

    // analysis
    TB_API TB_Predeccesors tb_get_predeccesors(TB_Function* f);
    TB_API TB_DominanceFrontiers tb_get_dominance_frontiers(TB_Function* f, TB_Predeccesors p, const TB_Label* doms);
    TB_API void tb_free_dominance_frontiers(TB_Function* f, TB_DominanceFrontiers* frontiers);

    typedef struct {
        size_t count;

        // max size is label_count
        TB_Label* traversal;

        // NOTE: you can free visited once the postorder calculation is complete
        // max size is label_count
        bool* visited;
    } TB_PostorderWalk;

    // Allocates from the heap and requires freeing with tb_function_free_postorder
    TB_API TB_PostorderWalk tb_function_get_postorder(TB_Function* f);
    TB_API void tb_function_free_postorder(TB_PostorderWalk* walk);

    // Doesn't allocate memory for you
    TB_API void tb_function_get_postorder_explicit(TB_Function* f, TB_PostorderWalk* walk);

    // if out_doms is NULL it'll only return the dominator array length (it's just the label count really)
    TB_API size_t tb_get_dominators(TB_Function* f, TB_Predeccesors p, TB_Label* out_doms);
    TB_API bool tb_is_dominated_by(TB_Label* doms, TB_Label expected_dom, TB_Label bb);

    TB_API TB_LoopInfo tb_get_loop_info(TB_Function* f, TB_Predeccesors preds, TB_Label* doms);
    TB_API void tb_free_loop_info(TB_LoopInfo loops);

    ////////////////////////////////
    // Transformation pass library
    ////////////////////////////////
    TB_API const TB_Pass tb_opt_merge_rets;
    TB_API const TB_Pass tb_opt_libcalls;
    TB_API const TB_Pass tb_opt_mem2reg;

    ////////////////////////////////
    // IR access
    ////////////////////////////////
    TB_API const char* tb_node_get_name(TB_Node* n);

    TB_API bool tb_is_expr_like(TB_Node* n);

    TB_API TB_Node* tb_node_get_last_register(TB_Function* f);
    TB_API TB_Node* tb_node_get_previous(TB_Function* f, TB_Node* at);

    TB_API TB_Node* tb_node_get_first_insertion_point(TB_Function* f, TB_Label bb);
    TB_API int tb_function_get_label_count(TB_Function* f);

    TB_API bool tb_node_is_constant_non_zero(TB_Node* n);
    TB_API bool tb_node_is_constant_zero(TB_Node* n);

    ////////////////////////////////
    // Format parsing
    ////////////////////////////////
    // We do this to parse the header
    bool tb_archive_parse(TB_Slice file, TB_ArchiveFileParser* restrict out_parser);
    // After that we can enumerate any symbol entries to resolve imports
    size_t tb_archive_parse_entries(TB_ArchiveFileParser* restrict parser, size_t i, size_t count, TB_ArchiveEntry* out_entry);

    TB_ObjectFile* tb_object_parse_coff(const TB_Slice file);
    void tb_object_free(TB_ObjectFile* obj);

    ////////////////////////////////
    // Test suite
    ////////////////////////////////
    #ifdef TB_COMPILE_TESTS
    bool tb_x64_test_suite(void);
    // bool tb_aarch64_test_suite(void);
    #endif /* TB_COMPILE_TESTS */

    #ifdef __cplusplus
}
#endif
#endif /* TB_CORE_H */