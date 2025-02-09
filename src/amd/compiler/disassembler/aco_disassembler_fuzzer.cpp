/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_disassembler.h"

#include "llvm/ac_llvm_util.h"

#include "util/memstream.h"

#include "llvm-c/Disassembler.h"

using namespace aco;

enum disassembler {
   disassembler_none = 0,
   disassembler_aco = 1,
   disassembler_llvm = 2,
};

static disassembler current_disassembler = disassembler_aco;

static void
handle_signal(int signal)
{
   exit(current_disassembler);
}

int
main(int argc, char** argv)
{
   uint32_t instruction_count = UINT32_MAX;

   uint32_t seed = 0;
   if (argc > 1)
      seed = strtoul(argv[1], NULL, 10);

   if (argc > 2) {
      instruction_count = strtoul(argv[2], NULL, 10);
   } else {
      for (uint32_t i = 0; i < instruction_count; i++) {
         uint32_t batch_size = 100;

         char cmd[256];
         snprintf(cmd, sizeof(cmd), "%s %u %u", argv[0], i, batch_size);
         FILE* p = popen(cmd, "r");
         if (p) {
            char line[256];
            while (fgets(line, sizeof(line), p)) {
               printf("%s", line);
            }
         }
         disassembler crashing_disassembler = (disassembler)WEXITSTATUS(pclose(p));
         if (crashing_disassembler == disassembler_aco)
            printf("The aco disassembler crashed! args: %u %u\n", i, batch_size);
         else if (crashing_disassembler == disassembler_llvm)
            printf("The llvm disassembler crashed! args: %u %u\n", i, batch_size);
      }
   }

   signal(SIGSEGV, handle_signal);
   signal(SIGILL, handle_signal);

   srandom(seed);

   disasm_context ctx;

   Program program;
   program.gfx_level = GFX11;
   program.family = CHIP_NAVI31;
   program.blocks.emplace_back();
   program.wave_size = 64;

   disasm_context_init(ctx, &program, stdout);

   const char* features = "";
   if (program.gfx_level >= GFX10 && program.wave_size == 64) {
      features = "+wavefrontsize64";
   }

   ac_init_llvm_once();

   LLVMDisasmContextRef disasm =
      LLVMCreateDisasmCPUFeatures("amdgcn-mesa-mesa3d", ac_get_llvm_processor_name(program.family),
                                  features, NULL, 0, NULL, NULL);

   for (uint32_t i = 0; i < instruction_count; i++) {
      uint32_t dwords[8];
      for (uint32_t j = 0; j < 8; j++)
         dwords[j] = rand();

      char llvm_string[256];

      current_disassembler = disassembler_llvm;
      size_t l = LLVMDisasmInstruction(disasm, (uint8_t*)dwords, 8 * sizeof(uint32_t), 0,
                                       llvm_string, sizeof(llvm_string));
      if (!l)
         continue;

      size_t disasm_size = 0;
      char* aco_disasm = NULL;
      struct u_memstream mem;
      if (!u_memstream_open(&mem, &aco_disasm, &disasm_size))
         return 1;

      ctx.output = u_memstream_get(&mem);
      fprintf(ctx.output, "\t");
      current_disassembler = disassembler_aco;
      disasm_instr(ctx, dwords, 0);

      u_memstream_close(&mem);

      /* Skip instructions where llvm is complaining to remove noise from the output. */
      if (strstr(llvm_string, "Invalid") || strstr(llvm_string, "Warning")) {
         free(aco_disasm);
         continue;
      }

      /* llvm prints the vcc dst which is unnecessary and adds noise to the output.
       * llvm also cannot decide if the op is named t or tru.
       */
      if (strstr(llvm_string, "v_cmp_t_") || strstr(llvm_string, "v_cmp_f_") ||
          strstr(llvm_string, "v_cmp_tru_") || strstr(llvm_string, "v_cmpx_tru_")) {
         free(aco_disasm);
         continue;
      }

      /* The aco disassembler always prints opsel, even if everything uses low bits. */
      if (strstr(aco_disasm, ".l")) {
         free(aco_disasm);
         continue;
      }

      if (strcmp(llvm_string, aco_disasm)) {
         fprintf(stdout, "args: %u %u\nllvm: %s\naco:  %s\n\n", seed, instruction_count,
                 llvm_string, aco_disasm);
      }

      free(aco_disasm);
   }

   return disassembler_none;
}
