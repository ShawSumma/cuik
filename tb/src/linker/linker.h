#pragma once
#include "../tb_internal.h"

#define NL_STRING_MAP_INLINE
#include <string_map.h>

// we use a linked list to store these because i couldn't be bothered to allocate
// one giant sequential region for the entire linker.
struct TB_LinkerSectionPiece {
    TB_LinkerSectionPiece* next;

    enum {
        // write the data buffer in this struct
        PIECE_NORMAL,
        // Write the TB module's text section
        PIECE_TEXT,
        // Write the TB module's data section
        PIECE_DATA,
        // Write the TB module's rdata section
        PIECE_RDATA,
        // Write the TB module's pdata section
        PIECE_PDATA,
        // Write the TB module's reloc section
        PIECE_RELOC,
        // Write the object file's reloc section
        PIECE_RELOC2
    } kind;

    TB_Module* module;
    TB_LinkerSection* parent;

    // vsize is the virtual size
    size_t offset, vsize, size;
    // 1 means it's immutable
    uint32_t flags;
    const uint8_t* data;
};

typedef enum {
    TB_LINKER_SECTION_DISCARD = 1,
    TB_LINKER_SECTION_COMDAT = 2,
} TB_LinkerSectionFlags;

struct TB_LinkerSection {
    NL_Slice name;

    TB_LinkerSectionFlags generic_flags;
    uint32_t flags;

    size_t address; // usually a relative virtual address.
    size_t offset;  // in the file.

    size_t total_size;
    TB_LinkerSectionPiece *first, *last;
};

typedef enum TB_LinkerSymbolTag {
    // external linkage
    TB_LINKER_SYMBOL_NORMAL,

    // used for windows stuff as "__ImageBase"
    TB_LINKER_SYMBOL_IMAGEBASE,

    // TB defined
    TB_LINKER_SYMBOL_TB,

    // imported from shared object
    TB_LINKER_SYMBOL_IMPORT,
} TB_LinkerSymbolTag;

typedef struct {
    TB_Slice name;
    // this is the location the thunk will call
    uint32_t ds_address;
    // this is the ID of the thunk
    uint32_t thunk_id;
    uint16_t ordinal;
} ImportThunk;

typedef enum TB_LinkerSymbolFlags {
    TB_LINKER_SYMBOL_WEAK = 1,
} TB_LinkerSymbolFlags;

// all symbols appended to the linker are converted into
// these and used for all kinds of relocation resolution.
typedef struct TB_LinkerSymbol {
    // key
    TB_Slice name;

    // value
    TB_LinkerSymbolTag tag;
    TB_LinkerSymbolFlags flags;
    TB_Slice object_name;
    union {
        // for normal symbols,
        struct {
            TB_LinkerSectionPiece* piece;
            uint32_t secrel;
        } normal;

        struct {
            uint32_t rva;
        } imagebase;

        // for IR module symbols
        struct {
            TB_LinkerSectionPiece* piece;
            TB_Symbol* sym;
        } tb;

        // for imports, refers to the imports array in TB_Linker
        struct {
            uint32_t id;
            uint16_t ordinal;
            ImportThunk* thunk;
        } import;
    };
} TB_LinkerSymbol;

// MSI hash table
typedef struct TB_SymbolTable {
    size_t exp, len;
    TB_LinkerSymbol* ht; // [1 << exp]
} TB_SymbolTable;

typedef struct {
    TB_Slice libpath;
    DynArray(ImportThunk) thunks;

    uint64_t *iat, *ilt;
} ImportTable;

typedef struct {
    TB_ObjectRelocType type;
    int addend;

    // flags
    bool is_thunk : 1;
    bool is_weak  : 1;

    // if target is NULL, check name
    TB_LinkerSymbol* target;
    TB_Slice name;

    struct {
        TB_LinkerSectionPiece* piece;
        size_t offset;
    } source;

    TB_Slice obj_name;
} TB_LinkerReloc;

// Format-specific vtable:
typedef struct TB_LinkerVtbl {
    void(*init)(TB_Linker* l);
    void(*append_object)(TB_Linker* l, TB_Slice obj_name, TB_ObjectFile* obj);
    void(*append_library)(TB_Linker* l, TB_Slice ar_file);
    void(*append_module)(TB_Linker* l, TB_Module* m);
    TB_Exports(*export)(TB_Linker* l);
} TB_LinkerVtbl;

typedef struct TB_UnresolvedSymbol TB_UnresolvedSymbol;
struct TB_UnresolvedSymbol {
    TB_UnresolvedSymbol* next;

    TB_Slice name;
    // if ext == NULL then use reloc
    TB_External* ext;
    TB_LinkerReloc* reloc;
};

typedef struct TB_Linker {
    TB_Arch target_arch;

    ptrdiff_t entrypoint; // -1 if not available
    NL_Strmap(TB_LinkerSection*) sections;

    // for relocations
    DynArray(TB_LinkerReloc) relocations;
    DynArray(TB_Module*) ir_modules;
    TB_SymbolTable symtab;

    size_t trampoline_pos;  // relative to the .text section
    TB_Emitter trampolines; // these are for calling imported functions

    NL_Strmap(TB_UnresolvedSymbol*) unresolved_symbols;

    // Windows specific:
    //   on windows, we use DLLs to interact with the OS so
    //   there needs to be a way to load these immediately,
    //   imports do just that.
    TB_LinkerSymbol* tls_index_sym;
    uint32_t iat_pos;
    DynArray(ImportTable) imports;

    TB_LinkerVtbl vtbl;
} TB_Linker;

// Error handling
TB_UnresolvedSymbol* tb__unresolved_symbol(TB_Linker* l, TB_Slice name);

// TB helpers
size_t tb__get_symbol_pos(TB_Symbol* s);
size_t tb__layout_text_section(TB_Module* m);

ImportThunk* tb__find_or_create_import(TB_Linker* l, TB_LinkerSymbol* restrict sym);

// Symbol table
TB_LinkerSymbol* tb__find_symbol(TB_SymbolTable* restrict symtab, TB_Slice name);
TB_LinkerSymbol* tb__append_symbol(TB_SymbolTable* restrict symtab, const TB_LinkerSymbol* sym);
uint64_t tb__compute_rva(TB_Linker* l, TB_Module* m, const TB_Symbol* s);
uint64_t tb__get_symbol_rva(TB_Linker* l, TB_LinkerSymbol* sym);

// Section management
void tb__merge_sections(TB_Linker* linker, TB_LinkerSection* from, TB_LinkerSection* to);

TB_LinkerSection* tb__find_section(TB_Linker* linker, const char* name, uint32_t flags);
TB_LinkerSection* tb__find_or_create_section(TB_Linker* linker, const char* name, uint32_t flags);
TB_LinkerSection* tb__find_or_create_section2(TB_Linker* linker, size_t name_len, const uint8_t* name_str, uint32_t flags);
TB_LinkerSectionPiece* tb__append_piece(TB_LinkerSection* section, int kind, size_t size, const void* data, TB_Module* mod);

size_t tb__pad_file(uint8_t* output, size_t write_pos, char pad, size_t align);
void tb__apply_external_relocs(TB_Linker* l, TB_Module* m, uint8_t* output);
size_t tb__apply_section_contents(TB_Linker* l, uint8_t* output, size_t write_pos, TB_LinkerSection* text, TB_LinkerSection* data, TB_LinkerSection* rdata, size_t section_alignment);
