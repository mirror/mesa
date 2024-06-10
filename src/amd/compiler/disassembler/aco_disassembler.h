/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ACO_DISASSEMBLER_H
#define ACO_DISASSEMBLER_H

#include "aco_ir.h"

#include "util/bitset.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aco {

bool disasm_program(Program* program, std::vector<uint32_t>& binary, uint32_t exec_size,
                    char** string);

struct instr_context;

typedef void (*disasm_instr_cb)(instr_context& ctx);

struct encoding_info {
   amd_gfx_level min_gfx_level;
   amd_gfx_level max_gfx_level;
   uint32_t encoding;
   uint32_t encoding_bitsize;
   uint32_t size;
   disasm_instr_cb disasm;
};

struct disasm_context {
   Program* program;
   std::vector<bool> referenced_blocks;
   const int16_t* opcode_encodings;
   std::unordered_map<Format, std::unordered_map<uint16_t, uint16_t>> opcodes;
   std::unordered_map<uint16_t, uint16_t> vop3_opcodes;
   std::unordered_map<aco_opcode, const char*> opcode_renames;
   std::vector<encoding_info> encoding_infos;
   std::unordered_map<uint32_t, uint32_t> block_offsets;
   BITSET_DECLARE(float_ops, (uint32_t)aco_opcode::num_opcodes);
   FILE* output;
};

struct instr_context {
   const disasm_context* disasm;
   const encoding_info* encoding;
   uint32_t* dwords;
   uint32_t instr_offset;
   uint32_t total_size;
   uint16_t op;
   Format format;
   Format encoded_format;
   bool printed_operand;
   bool has_def;
   bool has_sdwa;
   bool has_dpp8;
   bool has_dpp8_fi;
   bool has_dpp16;
   bool has_literal;
};

enum {
   vgpr = 0x100,
};

struct additional_operand_info {
   bool skip_comma;
   bool skip_null;
   uint32_t min_count;
   uint32_t count;
   bool neg;
   bool abs;
   bool tfe;
};

enum {
   operand_index_def = (1u << 31u),
};

struct op_rename {
   amd_gfx_level min_gfx_level;
   aco_opcode op;
   const char* name;
};

void disasm_context_init(disasm_context& ctx, Program* program, FILE* output);

size_t disasm_instr(const disasm_context& ctx, uint32_t* dwords, uint32_t instr_offset);

/* Disassembly helpers used by all generations: */

uint32_t bfe(const instr_context& ctx, uint32_t start, uint32_t count);
uint32_t bfe(uint32_t dword, uint32_t start, uint32_t count);

int32_t u2i(uint32_t word, uint32_t bitsize);

#define require_eq(ctx, field, value, expected)                                                    \
   if (value != expected)                                                                          \
   fprintf(ctx.disasm->output, " (invalid " #field " value %u, expected %u)", value, expected)

bool print_flag(instr_context& ctx, const char* name, uint32_t bit);
void print_integer_array(instr_context& ctx, const char* name, uint32_t* data, uint32_t length,
                         uint32_t ignored);
void print_mimg_dim(instr_context& ctx, ac_image_dim dim);

PhysReg parse_reg_src(instr_context& ctx, uint32_t reg);

uint32_t get_mimg_coord_components(instr_context& ctx, aco_mimg_op_info info, ac_image_dim dim,
                                   bool a16);

uint32_t mem_get_data_size(instr_context& ctx);
bool mem_has_dst(instr_context& ctx);
bool mem_has_conditional_dst(instr_context& ctx);
bool mem_has_data(instr_context& ctx);
bool mem_has_data2(instr_context& ctx);

void print_opcode(instr_context& ctx, Format format, uint16_t opcode);

bool print_operand(instr_context& ctx, uint32_t operand, uint32_t index,
                   std::optional<additional_operand_info> additional_info = std::nullopt);

void print_definition(instr_context& ctx, uint32_t def);

/* Generation specific instruction format handling: */

void disasm_smem_gfx8(instr_context& ctx);

void disasm_smem_gfx10(instr_context& ctx);
void disasm_mtbuf_gfx10(instr_context& ctx);
void disasm_mimg_gfx10(instr_context& ctx);

void disasm_vopd(instr_context& ctx);
void disasm_mubuf_gfx11(instr_context& ctx);
void disasm_mtbuf_gfx11(instr_context& ctx);
void disasm_mimg_gfx11(instr_context& ctx);
void disasm_flatlike_gfx11(instr_context& ctx);
void disasm_vinterp(instr_context& ctx);
void disasm_ldsdir(instr_context& ctx);

void print_cache_flags_gfx12(instr_context& ctx, uint32_t bit);
void disasm_mubuf_gfx12(instr_context& ctx);
void disasm_mtbuf_gfx12(instr_context& ctx);
void disasm_mimg_gfx12(instr_context& ctx);
void disasm_flatlike_gfx12(instr_context& ctx);

/* Tables: */

extern const std::unordered_set<aco_opcode> smem_buffer_ops;

extern const char* formats_gfx11[];

} // namespace aco

#endif
