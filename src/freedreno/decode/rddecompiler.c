/*
 * Copyright © 2022 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "util/u_math.h"

#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "freedreno_pm4.h"

#include "a6xx.xml.h"
#include "common/freedreno_dev_info.h"

#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/ralloc.h"
#include "util/rb_tree.h"
#include "util/set.h"
#include "util/u_vector.h"
#include "buffers.h"
#include "cffdec.h"
#include "disasm.h"
#include "io.h"
#include "rdutil.h"
#include "redump.h"
#include "rnnutil.h"

/* Decompiles a single cmdstream from .rd into compilable C source.
 * C source could be compiled using rdcompiler-meson.build as an example.
 *
 * For how-to see freedreno.rst
 */

static int handle_file(const char *filename, uint32_t submit_to_decompile);
static int emit_input_resources_txt(void);
static int emit_generate_rd_resources_h(void);

static const char *levels[] = {
   "\t",
   "\t\t",
   "\t\t\t",
   "\t\t\t\t",
   "\t\t\t\t\t",
   "\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t\t\t",
};

static struct {
   struct {
      int no_reg_bunch;
      bool split_into_files;
   } options;

   struct rnn *rnn;
   struct fd_dev_id dev_id;
   void *mem_ctx;

   struct set decompiled_shaders;

   FILE *out_file;
   int out_dir_fd; /* Only valid if `split_into_files. */
   unsigned ib_file_count; /* Only valid if `split_into_files. */
} rddc_ctx;

/* clang-format off */
static const struct option opts[] = {
      { "multi",        required_argument,   0, 'm' },
      { "submit",       required_argument,   0, 's' },
      { "no-reg-bunch", no_argument,         &rddc_ctx.options.no_reg_bunch, 1 },
      { "help",         no_argument,         0, 'h' },
};
/* clang-format on */

#define emit(...) fprintf(rddc_ctx.out_file, __VA_ARGS__)

static void
emitlvl(int lvl, const char *fmt, ...)
{
   assert(lvl < ARRAY_SIZE(levels));

   va_list args;
   va_start(args, fmt);
   fputs(levels[lvl], rddc_ctx.out_file);
   vfprintf(rddc_ctx.out_file, fmt, args);
   va_end(args);
}

static void
print_usage(const char *name)
{
   /* clang-format off */
   fprintf(stderr, "Usage:\n\n"
           "\t%s [OPTIONS]... FILE...\n\n"
           "Options:\n"
           "\t-m, --multi=<dir>   - Split into multiple translation units\n"
           "\t-s, --submit=№      - № of the submit to decompile\n"
           "\t--no-reg-bunch      - Use pkt4 for each reg in CP_CONTEXT_REG_BUNCH\n"
           "\t-h, --help          - show this message\n"
           , name);
   /* clang-format on */
}

static FILE *
fopen_output_file(const char *name)
{
   assert(rddc_ctx.options.split_into_files);

   int fd;
   errno = 0;
   fd = openat(rddc_ctx.out_dir_fd, name, O_CREAT | O_EXCL | O_WRONLY, 0600);
   if (fd < 0 && errno == EEXIST) {
      fprintf(stderr, "File already exists: %s\n", name);
      return NULL;
   } else if (fd < 0) {
      fprintf(stderr, "Failed to create file: %s\n", name);
      return NULL;
   }

   FILE *file = fdopen(fd, "w");
   if (!file) {
      close(fd);
      fprintf(stderr, "Failed to create file: %s\n", name);
      return NULL;
   }

   return file;
}

int
main(int argc, char **argv)
{
   int ret = -1;
   int c;
   uint32_t submit_to_decompile = -1;

   rddc_ctx.out_file = stdout;
   rddc_ctx.options.split_into_files = false;
   rddc_ctx.out_dir_fd = -1;

   while ((c = getopt_long(argc, argv, "m:s:h", opts, NULL)) != -1) {
      switch (c) {
      case 0:
         /* option that set a flag, nothing to do */
         break;
      case 'm':
         rddc_ctx.options.split_into_files = true;

         errno = 0;
         ret = mkdir(optarg, 0700);
         if (ret && errno != EEXIST) {
            fprintf(stderr, "Failed to create the output directory: %s.\n",
                    optarg);
            goto err_return;
         }

         rddc_ctx.out_dir_fd = open(optarg, O_PATH | O_DIRECTORY);
         if (rddc_ctx.out_dir_fd < 0) {
            fprintf(stderr, "Failed to open the output directory: %s.\n",
                    optarg);
            goto err_return;
         }

         rddc_ctx.ib_file_count = 0;
         break;
      case 's':
         submit_to_decompile = strtoul(optarg, NULL, 0);
         break;
      case 'h':
      default:
         goto err_return;
      }
   }

   if (submit_to_decompile == -1) {
      fprintf(stderr, "Submit to decompile has to be specified\n");
      goto err_close_out_dir_fd;
   }

   if (rddc_ctx.options.split_into_files) {
      const char *out_file_name = "generate-rd.cc";
      rddc_ctx.out_file = fopen_output_file(out_file_name);
      if (!rddc_ctx.out_file)
         goto err_close_out_dir_fd;
   } else {
      rddc_ctx.out_file = stdout;
   }

   if (optind + 1 != argc) {
      fprintf(stderr, "Multiple RD input files specified or none\n");
      goto err_close_out_file;
   }

   ret = handle_file(argv[optind], submit_to_decompile);
   if (ret)
      goto err_close_out_file;

   optind++;

   if (rddc_ctx.options.split_into_files) {
      ret = emit_input_resources_txt();
      if (ret)
         goto err_close_out_file;

      ret = emit_generate_rd_resources_h();
      if (ret)
         goto err_close_out_file;
   }

   if (rddc_ctx.out_file != stdout)
      fclose(rddc_ctx.out_file);

   if (rddc_ctx.out_dir_fd >= 0)
      close(rddc_ctx.out_dir_fd);

   return 0;

err_close_out_file:
   if (rddc_ctx.out_file != stdout)
      fclose(rddc_ctx.out_file);

err_close_out_dir_fd:
   if (rddc_ctx.out_dir_fd >= 0)
      close(rddc_ctx.out_dir_fd);

err_return:
   print_usage(argv[0]);
   return EXIT_FAILURE;
}

static void
init_rnn(const char *gpuname)
{
   rddc_ctx.rnn = rnn_new(true);
   rnn_load(rddc_ctx.rnn, gpuname);
}

const char *
pktname(unsigned opc)
{
   return rnn_enumname(rddc_ctx.rnn, "adreno_pm4_type3_packets", opc);
}

enum name_type {
   SHADER_ASM_STR,
   SHADER_SRC_FILE,
   IB_FUNC,
   IB_FUNC_PROTOTYPE,
   IB_SRC_FILE,
};

static char *
gen_name(enum name_type name_type, uint64_t key)
{
   char name[100];

   switch (name_type) {
   case SHADER_ASM_STR:
      sprintf(name, "shader_source_%016" PRIx64, key);
      break;
   case SHADER_SRC_FILE:
      sprintf(name, "generate-rd-shader-%016" PRIx64 ".cc", key);
      break;
   case IB_FUNC:
      sprintf(name, "ib_%" PRIu64, key);
      break;
   case IB_FUNC_PROTOTYPE:
      sprintf(name,
              "void ib_%" PRIu64
              "(struct replay_context *ctx, struct cmdstream *cs)",
              key);
      break;
   case IB_SRC_FILE:
      sprintf(name, "generate-rd-ib-%" PRIu64 ".cc", key);
      break;
   }

   return strdup(name);
}

static int
emit_input_resources_txt(void)
{
   const char *file_name = "input_resources.txt";
   int ret;

   FILE *stream = fopen_output_file(file_name);
   if (!stream)
      return -1;

   ret = fprintf(stream, "[\n");
   if (ret < 0) {
      fprintf(stderr, "Failed writing to %s\n", file_name);
      fclose(stream);
      return -1;
   }

   set_foreach (&rddc_ctx.decompiled_shaders, entry) {
      const uint64_t key = *(uint64_t *)entry->key;
      char *shader_file_name = gen_name(SHADER_SRC_FILE, key);

      int ret = fprintf(stream, "'%s',\n", shader_file_name);
      if (ret < 0) {
         fprintf(stderr, "Failed writing \"%s\"to %s\n", shader_file_name,
                 file_name);
         free(shader_file_name);
         fclose(stream);
         return -1;
      }

      free(shader_file_name);
   }

   for (unsigned id = 0; id < rddc_ctx.ib_file_count; id++) {
      char *ib_file_name = gen_name(IB_SRC_FILE, id);

      int ret = fprintf(stream, "'%s',\n", ib_file_name);
      if (ret < 0) {
         fprintf(stderr, "Failed writing \"%s\"to %s\n", ib_file_name,
                 file_name);
         free(ib_file_name);
         fclose(stream);
         return -1;
      }

      free(ib_file_name);
   }

   ret = fprintf(stream, "],");
   if (ret < 0) {
      fprintf(stderr, "Failed writing to %s\n", file_name);
      fclose(stream);
      return -1;
   }

   fclose(stream);

   return 0;
}

static int
emit_generate_rd_resources_h(void)
{
   const char *file_name = "generate-rd-resources.h";

   FILE *stream = fopen_output_file(file_name);
   if (!stream)
      return -1;

   for (unsigned id = 0; id < rddc_ctx.ib_file_count; id++) {
      char *ib_func = gen_name(IB_FUNC_PROTOTYPE, id);

      int ret = fprintf(stream, "%s;\n", ib_func);
      if (ret < 0) {
         fprintf(stderr, "Failed writing \"%s\"to %s\n", ib_func, file_name);
         free(ib_func);
         fclose(stream);
         return -1;
      }

      free(ib_func);
   }

   set_foreach (&rddc_ctx.decompiled_shaders, entry) {
      const uint64_t key = *(uint64_t *)entry->key;
      char *shader_name = gen_name(SHADER_ASM_STR, key);

      int ret = fprintf(stream, "const char *get_%s(void);\n", shader_name);
      if (ret < 0) {
         fprintf(stderr, "Failed writing \"%s\"to %s\n", shader_name,
                 file_name);
         free(shader_name);
         fclose(stream);
         return -1;
      }

      free(shader_name);
   }

   fclose(stream);

   return 0;
}

static uint32_t
decompile_shader(const char *name, uint32_t regbase, uint32_t *dwords, int level)
{
   uint64_t gpuaddr = ((uint64_t)dwords[1] << 32) | dwords[0];
   gpuaddr &= 0xfffffffffffffff0;

   /* Shader's iova is referenced in two places, so we have to remember it. */
   if (_mesa_set_search(&rddc_ctx.decompiled_shaders, &gpuaddr)) {
      emitlvl(level, "emit_shader_iova(ctx, cs, 0x%" PRIx64 ");\n", gpuaddr);
   } else {
      uint64_t *key = ralloc(rddc_ctx.mem_ctx, uint64_t);
      *key = gpuaddr;
      _mesa_set_add(&rddc_ctx.decompiled_shaders, key);

      void *buf = hostptr(gpuaddr);
      assert(buf);

      uint32_t sizedwords = hostlen(gpuaddr) / 4;

      char *stream_data = NULL;
      size_t stream_size = 0;
      FILE *stream = open_memstream(&stream_data, &stream_size);

      try_disasm_a3xx(buf, sizedwords, 0, stream,
                      fd_dev_gen(&rddc_ctx.dev_id) * 100);
      fclose(stream);

      char *shader_name = gen_name(SHADER_ASM_STR, *key);

      emitlvl(level, "{\n");

      if (rddc_ctx.out_dir_fd >= 0) {
         char *shader_file_name = gen_name(SHADER_SRC_FILE, *key);
         FILE *shader_file = fopen_output_file(shader_file_name);

         fprintf(shader_file,
                 "static const char *%s = R\"(\n"
                 "%s)\";\n"
                 "const char *get_%s(void) { return %s; }\n",
                 shader_name, stream_data, shader_name, shader_name);

         fclose(shader_file);
         free(shader_file_name);

         emitlvl(level + 1, "upload_shader(ctx, 0x%" PRIx64 ", get_%s());\n",
                 gpuaddr, shader_name);
      } else {
         emitlvl(level + 1, "const char *%s = R\"(\n", shader_name);
         emit("%s", stream_data);
         emitlvl(level + 1, ")\";\n");
         emitlvl(level + 1, "upload_shader(ctx, 0x%" PRIx64 ", %s);\n", gpuaddr,
                 shader_name);
      }

      emitlvl(level + 1, "emit_shader_iova(ctx, cs, 0x%" PRIx64 ");\n",
              gpuaddr);
      emitlvl(level, "}\n");

      free(stream_data);
      free(shader_name);
   }

   return 2;
}

static struct {
   uint32_t regbase;
   uint32_t (*fxn)(const char *name, uint32_t regbase, uint32_t *dwords, int level);
} reg_a6xx[] = {
   {REG_A6XX_SP_VS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_HS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_DS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_GS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_FS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_CS_OBJ_START, decompile_shader},

   {0, NULL},
}, *type0_reg;

static uint32_t
decompile_register(uint32_t regbase, uint32_t *dwords, uint16_t cnt, int level)
{
   struct rnndecaddrinfo *info = rnn_reginfo(rddc_ctx.rnn, regbase);

   for (unsigned idx = 0; type0_reg[idx].regbase; idx++) {
      if (type0_reg[idx].regbase == regbase) {
         return type0_reg[idx].fxn(info->name, regbase, dwords, level);
      }
   }

   const uint32_t dword = *dwords;

   if (info && info->typeinfo) {
      char *decoded = rnndec_decodeval(rddc_ctx.rnn->vc, info->typeinfo, dword);
      emitlvl(level, "/* pkt4: %s = %s */\n", info->name, decoded);

      if (cnt == 0) {
         emitlvl(level, "pkt(cs, 0x%x);\n", dword);
      } else {
#if 0
         char reg_name[33];
         char field_name[33];
         char reg_idx[33];

         /* reginfo doesn't return reg name in a compilable format, for now just
          * parse it into a compilable reg name.
          * TODO: Make RNN optionally return compilable reg name.
          */
         if (sscanf(info->name, "%32[A-Z0-6_][%32[x0-9]].%32s", reg_name,
                    reg_idx, field_name) != 3) {
            emitlvl(level, "pkt4(cs, REG_%s_%s, (%u), %u);\n", rnn->variant,
                     info->name, cnt, dword);
         } else {
            emitlvl(level, "pkt4(cs, REG_%s_%s_%s(%s), (%u), %u);\n",
                     rnn->variant, reg_name, field_name, reg_idx, cnt, dword);
         }
#else
         /* TODO: We don't have easy way to get chip generation prefix,
          * so just emit raw packet offset as a workaround.
          */
         emitlvl(level, "pkt4(cs, 0x%04x, (%u), 0x%x);\n", regbase, cnt, dword);
#endif
      }
   } else {
      emitlvl(level, "/* unknown pkt4 */\n");
      emitlvl(level, "pkt4(cs, 0x%04x, (%u), 0x%x);\n", regbase, 1, dword);
   }

   rnn_reginfo_free(info);

   return 1;
}

static uint32_t
decompile_register_reg_bunch(uint32_t regbase, uint32_t *dwords, uint16_t cnt, int level)
{
   struct rnndecaddrinfo *info = rnn_reginfo(rddc_ctx.rnn, regbase);
   const uint32_t dword = *dwords;

   if (info && info->typeinfo) {
      char *decoded = rnndec_decodeval(rddc_ctx.rnn->vc, info->typeinfo, dword);
      emitlvl(level, "/* reg: %s = %s */\n", info->name, decoded);
   } else {
      emitlvl(level, "/* unknown pkt4 */\n");
   }

   emitlvl(level, "pkt(cs, 0x%04x);\n", regbase);
   emitlvl(level, "pkt(cs, 0x%x);\n", dword);

   rnn_reginfo_free(info);

   return 1;
}

static void
decompile_registers(uint32_t regbase, uint32_t *dwords, uint32_t sizedwords,
                    int level)
{
   if (!sizedwords)
      return;
   uint32_t consumed = decompile_register(regbase, dwords, sizedwords, level);
   sizedwords -= consumed;
   while (sizedwords > 0) {
      regbase += consumed;
      dwords += consumed;
      consumed = decompile_register(regbase, dwords, 0, level);
      sizedwords -= consumed;
   }
}

static void
decompile_domain(uint32_t pkt, uint32_t *dwords, uint32_t sizedwords,
                 const char *dom_name, const char *packet_name, int level)
{
   struct rnndomain *dom;
   int i;

   dom = rnn_finddomain(rddc_ctx.rnn->db, dom_name);

   emitlvl(level, "pkt7(cs, %s, %u);\n", packet_name, sizedwords);

   if (pkt == CP_LOAD_STATE6_FRAG || pkt == CP_LOAD_STATE6_GEOM) {
      enum a6xx_state_type state_type =
         (dwords[0] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >>
         CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
      enum a6xx_state_src state_src =
         (dwords[0] & CP_LOAD_STATE6_0_STATE_SRC__MASK) >>
         CP_LOAD_STATE6_0_STATE_SRC__SHIFT;

      /* TODO: decompile all other state */
      if (state_type == ST6_SHADER && state_src == SS6_INDIRECT) {
         emitlvl(level, "pkt(cs, 0x%x);\n", dwords[0]);
         decompile_shader(NULL, 0, dwords + 1, level);
         return;
      }
   }

   for (i = 0; i < sizedwords; i++) {
      struct rnndecaddrinfo *info = NULL;
      if (dom) {
         info = rnndec_decodeaddr(rddc_ctx.rnn->vc, dom, i, 0);
      }

      char *decoded;
      if (!(info && info->typeinfo)) {
         emitlvl(level, "pkt(cs, 0x%x);\n", dwords[i]);
         continue;
      }
      uint64_t value = dwords[i];
      bool reg64 = info->typeinfo->high >= 32 && i < sizedwords - 1;
      if (reg64) {
         value |= (uint64_t)dwords[i + 1] << 32;
      }
      decoded = rnndec_decodeval(rddc_ctx.rnn->vc, info->typeinfo, value);

      emitlvl(level, "/* %s */\n", decoded);
      emitlvl(level, "pkt(cs, 0x%x);\n", dwords[i]);
      if (reg64) {
         emitlvl(level, "pkt(cs, 0x%x);\n", dwords[i + 1]);
         i++;
      }

      free(decoded);
      free(info->name);
      free(info);
   }
}

static void
decompile_commands(uint32_t *dwords, uint32_t sizedwords, int level, uint32_t *cond_count)
{
   int dwords_left = sizedwords;
   uint32_t count = 0; /* dword count including packet header */
   uint32_t val;

   if (!dwords) {
      fprintf(stderr, "NULL cmd buffer!\n");
      return;
   }

   while (dwords_left > 0) {
      if (pkt_is_regwrite(dwords[0], &val, &count)) {
         assert(val < 0xffff);
         decompile_registers(val, dwords + 1, count - 1, level);
      } else if (pkt_is_opcode(dwords[0], &val, &count)) {
         if (val == CP_INDIRECT_BUFFER) {
            uint64_t ibaddr;
            uint32_t ibsize;
            ibaddr = dwords[1];
            ibaddr |= ((uint64_t)dwords[2]) << 32;
            ibsize = dwords[3];

            emitlvl(level, "{\n");
            emitlvl(level + 1, "begin_ib(ctx);\n");

            /* Arbitrarily chosen limit. */
            if (ibsize > 512 && rddc_ctx.options.split_into_files) {
               const unsigned id = rddc_ctx.ib_file_count++;

               char *ib_func_name = gen_name(IB_FUNC, id);
               emitlvl(level + 1, "%s(ctx, cs);\n", ib_func_name);
               free(ib_func_name);

               char *ib_file_name = gen_name(IB_SRC_FILE, id);
               FILE *ib_file = fopen_output_file(ib_file_name);
               free(ib_file_name);

               FILE *old_out_file = rddc_ctx.out_file;
               rddc_ctx.out_file = ib_file;

               int old_level = level;
               level = 0;

               char *ib_func_prototype = gen_name(IB_FUNC_PROTOTYPE, id);

               /* Have to use emit. decompile_commands() will indent code. */
               emit("#include \"decode/rdcompiler-utils.h\"\n");
               emit("#include \"generate-rd-resources.h\"\n");
               emit("%s\n{\n", ib_func_prototype);

               free(ib_func_prototype);

               uint32_t *ptr = hostptr(ibaddr);
               decompile_commands(ptr, ibsize, level, NULL);

               emit("}\n");

               fclose(ib_file);

               rddc_ctx.out_file = old_out_file;
               level = old_level;
            } else {
               uint32_t *ptr = hostptr(ibaddr);
               decompile_commands(ptr, ibsize, level + 1, NULL);
            }

            emitlvl(level + 1, "end_ib();\n");
            emitlvl(level, "}\n");
         } else if (val == CP_SET_DRAW_STATE) {
            for (int i = 1; i < count; i += 3) {
               uint32_t state_count = dwords[i] & 0xffff;
               if (state_count != 0) {
                  uint32_t unchanged = dwords[i] & (~0xffff);
                  uint64_t ibaddr = dwords[i + 1];
                  ibaddr |= ((uint64_t)dwords[i + 2]) << 32;

                  emitlvl(level, "{\n");
                  emitlvl(level + 1, "begin_draw_state(ctx);\n");

                  uint32_t *ptr = hostptr(ibaddr);
                  decompile_commands(ptr, state_count, level + 1, NULL);

                  emitlvl(level + 1, "end_draw_state(ctx, %u);\n", unchanged);
                  emitlvl(level, "}\n");
               } else {
                  decompile_domain(val, dwords + i, 3, "CP_SET_DRAW_STATE",
                                   "CP_SET_DRAW_STATE", level);
               }
            }
         } else if (val == CP_CONTEXT_REG_BUNCH || val == CP_CONTEXT_REG_BUNCH2) {
            uint32_t *dw = dwords + 1;
            uint32_t cnt = count - 1;

            if (val == CP_CONTEXT_REG_BUNCH2) {
               if (rddc_ctx.options.no_reg_bunch) {
                  emitlvl(level, "// CP_CONTEXT_REG_BUNCH2\n");
                  emitlvl(level, "{\n");
               } else {
                  emitlvl(level, "pkt7(cs, %s, %u);\n", "CP_CONTEXT_REG_BUNCH2",
                          cnt);
                  emitlvl(level, "{\n");
                  emitlvl(level + 1, "pkt(cs, 0x%x);\n", dw[0]);
                  emitlvl(level + 1, "pkt(cs, 0x%x);\n", dw[1]);
               }

               dw += 2;
               cnt -= 2;
            } else {
               if (rddc_ctx.options.no_reg_bunch) {
                  emitlvl(level, "// CP_CONTEXT_REG_BUNCH\n");
                  emitlvl(level, "{\n");
               } else {
                  emitlvl(level, "pkt7(cs, %s, %u);\n", "CP_CONTEXT_REG_BUNCH",
                          cnt);
                  emitlvl(level, "{\n");
               }
            }

            for (uint32_t i = 0; i < cnt; i += 2) {
               if (rddc_ctx.options.no_reg_bunch) {
                  decompile_register(dw[i], &dw[i + 1], 1, level + 1);
               } else {
                  decompile_register_reg_bunch(dw[i], &dw[i + 1], 1, level + 1);
               }
            }
            emitlvl(level, "}\n");
         } else if (val == CP_COND_REG_EXEC) {
            const char *packet_name = pktname(val);
            const char *dom_name = packet_name;
            uint32_t cond_count = dwords[count - 1];

            decompile_domain(val, dwords + 1, count - 1, dom_name, packet_name, level);

            emitlvl(level, "{\n");
            emitlvl(level + 1, "/* BEGIN COND (%d DWORDS) */\n", cond_count);

            decompile_commands(dwords + count, cond_count, level + 1, &cond_count);
            count += cond_count;

            emitlvl(level + 1, "/* END COND */\n");
            emitlvl(level, "}\n");
         } else if (val == CP_NOP) {
            /* Prop will often use NOP past the end of cond execs
             * which basically create an else path for the cond exec
             */
            const char *packet_name = pktname(val);
            const char *dom_name = packet_name;

            if (count > dwords_left) {
               int else_cond_count = count - dwords_left;

               assert(cond_count);
               *cond_count += else_cond_count;

               emitlvl(level, "pkt7(cs, %s, %u);\n", packet_name, count - 1);
               for (int i = 1; i < dwords_left; i++) {
                  emitlvl(level, "pkt(cs, 0x%x);\n", dwords[i]);
               }

               emitlvl(level, "/* TO ELSE COND */\n");
               emitlvl(level - 1, "}\n");

               emitlvl(level - 1, "{\n");
               emitlvl(level, "/* ELSE COND (%d DWORDS) */\n", else_cond_count);
               decompile_commands(dwords + dwords_left, else_cond_count, level, NULL);

               return;
            } else {
               decompile_domain(val, dwords + 1, count - 1, dom_name, packet_name,
                                level);
            }
         } else {
            const char *packet_name = pktname(val);
            const char *dom_name = packet_name;
            if (packet_name) {
               /* special hack for two packets that decode the same way
                * on a6xx:
                */
               if (!strcmp(packet_name, "CP_LOAD_STATE6_FRAG") ||
                   !strcmp(packet_name, "CP_LOAD_STATE6_GEOM"))
                  dom_name = "CP_LOAD_STATE6";
               decompile_domain(val, dwords + 1, count - 1, dom_name, packet_name,
                                level);
            } else {
               errx(1, "unknown pkt7 %u", val);
            }
         }
      } else {
         errx(1, "unknown packet %u", dwords[0]);
      }

      dwords += count;
      dwords_left -= count;
   }

   if (dwords_left < 0)
      fprintf(stderr, "**** this ain't right!! dwords_left=%d\n", dwords_left);
}

static void
emit_header()
{
   if (!rddc_ctx.dev_id.gpu_id && !rddc_ctx.dev_id.chip_id)
      return;

   switch (fd_dev_gen(&rddc_ctx.dev_id)) {
   case 6:
      init_rnn("a6xx");
      break;
   case 7:
      init_rnn("a7xx");
      break;
   default:
      errx(-1, "unsupported gpu: %u", rddc_ctx.dev_id.gpu_id);
   }

   emit("#include \"decode/rdcompiler-utils.h\"\n");

   if (rddc_ctx.options.split_into_files)
      emit("#include \"generate-rd-resources.h\"\n");

   emit("int main(int argc, char **argv)\n"
        "{\n"
        "\tstruct replay_context _ctx;\n"
        "\tstruct replay_context *ctx = &_ctx;\n"
        "\tstruct fd_dev_id dev_id = {%u, 0x%" PRIx64 "};\n"
        "\treplay_context_init(ctx, &dev_id, argc, argv);\n"
        "\tstruct cmdstream *cs = ctx->submit_cs;\n\n",
        rddc_ctx.dev_id.gpu_id, rddc_ctx.dev_id.chip_id);
}

static inline uint32_t
u64_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(uint64_t));
}

static inline bool
u64_compare(const void *key1, const void *key2)
{
   return memcmp(key1, key2, sizeof(uint64_t)) == 0;
}

static int
handle_file(const char *filename, uint32_t submit_to_decompile)
{
   struct io *io;
   int submit = 0;
   bool needs_reset = false;
   struct rd_parsed_section ps = {0};

   if (!strcmp(filename, "-"))
      io = io_openfd(0);
   else
      io = io_open(filename);

   if (!io)
      return -1;

   type0_reg = reg_a6xx;
   rddc_ctx.mem_ctx = ralloc_context(NULL);
   _mesa_set_init(&rddc_ctx.decompiled_shaders, rddc_ctx.mem_ctx, u64_hash,
                  u64_compare);

   struct {
      unsigned int len;
      uint64_t gpuaddr;
   } gpuaddr = {0};

   while (parse_rd_section(io, &ps)) {
      switch (ps.type) {
      case RD_TEST:
      case RD_VERT_SHADER:
      case RD_FRAG_SHADER:
      case RD_CMD:
         /* no-op */
         break;
      case RD_GPUADDR:
         if (needs_reset) {
            reset_buffers();
            needs_reset = false;
         }

         parse_addr(ps.buf, ps.sz, &gpuaddr.len, &gpuaddr.gpuaddr);
         break;
      case RD_BUFFER_CONTENTS:
         add_buffer(gpuaddr.gpuaddr, gpuaddr.len, ps.buf);
         ps.buf = NULL;
         break;
      case RD_CMDSTREAM_ADDR: {
         unsigned int sizedwords;
         uint64_t gpuaddr;
         parse_addr(ps.buf, ps.sz, &sizedwords, &gpuaddr);

         if (submit == submit_to_decompile) {
            decompile_commands(hostptr(gpuaddr), sizedwords, 0, NULL);
         }

         needs_reset = true;
         submit++;
         break;
      }
      case RD_GPU_ID: {
         rddc_ctx.dev_id.gpu_id = parse_gpu_id(ps.buf);
         if (fd_dev_info_raw(&rddc_ctx.dev_id))
            emit_header();
         break;
      }
      case RD_CHIP_ID: {
         rddc_ctx.dev_id.chip_id = parse_chip_id(ps.buf);
         if (fd_dev_info_raw(&rddc_ctx.dev_id))
            emit_header();
         break;
      }
      default:
         break;
      }
   }

   emit("\treplay_context_finish(ctx);\n}");

   io_close(io);
   fflush(rddc_ctx.out_file);

   if (ps.ret < 0) {
      fprintf(stderr, "corrupt file\n");
   }
   return 0;
}
