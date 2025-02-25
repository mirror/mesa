/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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

#include "common/freedreno_dev_info.h"
#include "a6xx.xml.h"

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

/* Dumps registers, commands and raw shader instructions in a way that is
 * easily parseable and "fast" (rnn lookups aren't that fast).
 * Example of output format:
 *    0x002e	CP_SET_BIN_DATA5_OFFSET[0]	0x10000	{ VSC_SIZE = 1 | VSC_N = 0 }
 *    0x002e	CP_SET_BIN_DATA5_OFFSET[1]	0x2080	{ BIN_DATA_OFFSET = 8320 }
 *    0x002e	CP_SET_BIN_DATA5_OFFSET[2]	0x8	{ BIN_SIZE_OFFSET = 8 }
 *    0x80f0	GRAS_SC_WINDOW_SCISSOR_TL	0x240	{ X = 576 | Y = 0 }
 *    0x80f1	GRAS_SC_WINDOW_SCISSOR_BR	0x2cf02ff	{ X = 767 | Y = 719 }
 *    0xae08	0xae08	0x400
 *    0xae09	0xae09	0x430800
 *    0xae0a	0xae0a	0x0
 *    0xa81b	SP_VS_OBJ_FIRST_EXEC_OFFSET	0x0	0
 *    0x038200000000000a
 *    0x0282000000000009
 *    0xc02600fc00c78100
 */

static int handle_file(const char *filename);

static struct rnn *rnn;
static struct fd_dev_id dev_id;
static void *mem_ctx;

static struct set dumped_shaders;

static void
init_rnn(const char *gpuname)
{
   rnn = rnn_new(true);
   rnn_load(rnn, gpuname);
}

int
main(int argc, char **argv)
{
   int ret = -1;

   ret = handle_file(argv[1]);
   if (ret) {
      fprintf(stderr, "error reading: %s\n", argv[1]);
   }

   return ret;
}

const char *
pktname(unsigned opc)
{
   return rnn_enumname(rnn, "adreno_pm4_type3_packets", opc);
}

static void
print_raw_shader(uint32_t regbase, uint32_t *dwords)
{
   uint64_t gpuaddr = ((uint64_t)dwords[1] << 32) | dwords[0];
   gpuaddr &= 0xfffffffffffffff0;

   if (!_mesa_set_search(&dumped_shaders, &gpuaddr)) {
      uint64_t *key = ralloc(mem_ctx, uint64_t);
      *key = gpuaddr;
      _mesa_set_add(&dumped_shaders, key);

      uint64_t *buf = hostptr(gpuaddr);
      assert(buf);

      uint32_t instructions = hostlen(gpuaddr) / 8;

      for (uint32_t i = 0; i < instructions; i++) {
         if (buf[i] == 0x0300000000000000)
            break;

         printf("0x%016" PRIx64 "\n", buf[i]);
      }
   }
}

static uint32_t
print_register(uint32_t regbase, uint32_t *dwords, uint16_t cnt)
{
   struct rnndecaddrinfo *info = rnn_reginfo(rnn, regbase);

   static uint32_t shader_regs[] = {REG_A6XX_SP_VS_OBJ_START,
                                    REG_A6XX_SP_HS_OBJ_START,
                                    REG_A6XX_SP_DS_OBJ_START,
                                    REG_A6XX_SP_GS_OBJ_START,
                                    REG_A6XX_SP_FS_OBJ_START,
                                    REG_A6XX_SP_CS_OBJ_START,
                                    0};

   for (unsigned idx = 0; shader_regs[idx]; idx++) {
      if (shader_regs[idx] == regbase) {
         print_raw_shader(regbase, dwords);
         rnn_reginfo_free(info);
         return 2;
      }
   }

   const uint32_t dword = *dwords;
   uint32_t consumed = 1;

   if (info && info->typeinfo && info->width != 64) {
      char *decoded = rnndec_decodeval(rnn->vc, info->typeinfo, dword);
      printf("0x%04x\t%s\t0x%x\t%s\n", regbase, info->name, dword, decoded);
   } else {
      if (info->width == 64) {
         const uint64_t address = *dwords;
         printf("0x%04x\t%s\t0x%" PRIx64 "\tADDRESS64\n", regbase, info->name,
                address);
         consumed = 2;
      } else {
         printf("0x%04x\t%s\t0x%x\t \n", regbase, info->name, dword);
      }
   }

   rnn_reginfo_free(info);

   return consumed;
}

static void
print_registers(uint32_t regbase, uint32_t *dwords, int32_t sizedwords)
{
   if (!sizedwords)
      return;
   uint32_t consumed = print_register(regbase, dwords, sizedwords);
   sizedwords -= consumed;
   while (sizedwords > 0) {
      regbase += consumed;
      dwords += consumed;
      consumed = print_register(regbase, dwords, 0);
      sizedwords -= consumed;
   }
}

static bool
print_domain(uint32_t pkt, uint32_t *dwords, uint32_t sizedwords,
             const char *dom_name, const char *packet_name)
{
   struct rnndomain *dom;

   dom = rnn_finddomain(rnn->db, dom_name);

   if (!dom)
      return false;

   for (uint32_t i = 0; i < sizedwords; i++) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(rnn->vc, dom, i, 0);

      char *decoded;
      if (!(info && info->typeinfo))
         break;
      uint64_t value = dwords[i];
      if (info->typeinfo->high >= 32 && i < sizedwords - 1) {
         value |= (uint64_t)dwords[i + 1] << 32;
         i++; /* skip the next dword since we're printing it now */
      }
      decoded = rnndec_decodeval(rnn->vc, info->typeinfo, value);

      printf("0x%04x\t%s[%u]\t0x%" PRIx64 "\t%s\n", pkt, dom_name, i, value,
             decoded);

      free(decoded);
      free(info->name);
      free(info);
   }

   return true;
}

static void
print_commands(uint32_t *dwords, uint32_t sizedwords)
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
         print_registers(val, dwords + 1, count - 1);
      } else if (pkt_is_opcode(dwords[0], &val, &count)) {
         if (val == CP_INDIRECT_BUFFER) {
            uint64_t ibaddr;
            uint32_t ibsize;
            ibaddr = dwords[1];
            ibaddr |= ((uint64_t)dwords[2]) << 32;
            ibsize = dwords[3];

            if (!has_dumped(ibaddr, 0x7)) {
               uint32_t *ptr = hostptr(ibaddr);
               print_commands(ptr, ibsize);
            }
         } else if (val == CP_SET_DRAW_STATE) {
            for (int i = 1; i < count; i += 3) {
               uint32_t state_count = dwords[i] & 0xffff;
               if (state_count != 0) {
                  uint64_t ibaddr = dwords[i + 1];
                  ibaddr |= ((uint64_t)dwords[i + 2]) << 32;

                  uint32_t *ptr = hostptr(ibaddr);
                  print_commands(ptr, state_count);
               } else {
                  print_domain(val, dwords + i, 3, "CP_SET_DRAW_STATE",
                               "CP_SET_DRAW_STATE");
               }
            }
         } else if (val == CP_CONTEXT_REG_BUNCH ||
                    val == CP_CONTEXT_REG_BUNCH2) {
            uint32_t *dw = dwords + 1;
            uint32_t sz = count - 1;

            if (val == CP_CONTEXT_REG_BUNCH2) {
               dw += 2;
               sz -= 2;
            }

            for (uint32_t i = 0; i < sz; i += 2) {
               print_register(dw[i + 0], &dw[i + 1], 1);
            }
         } else {
            const char *packet_name = pktname(val);
            const char *dom_name = packet_name;
            bool dump_raw = !packet_name;

            if (packet_name) {
               if (!strcmp(packet_name, "CP_LOAD_STATE6_FRAG") ||
                   !strcmp(packet_name, "CP_LOAD_STATE6_GEOM"))
                  dom_name = "CP_LOAD_STATE6";

               dump_raw = !print_domain(val, dwords + 1, count - 1, dom_name,
                                        packet_name);
            }

            switch (val) {
            case CP_NOP:
            case CP_RESOURCE_LIST: {
               dump_raw = false;
               break;
            }
            }

            if (dump_raw) {
               char buf[255];
               if (!packet_name) {
                  sprintf(buf, "%s%x", "CP_UNK", val);
                  packet_name = buf;
               }

               for (uint32_t i = 0; i < (count - 1); i += 1) {
                  printf("0x%04x\t%s[%u]\t0x%x\t%s\n", val, packet_name, i,
                         dwords[i + 1], "");
               }
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

static void
init_gpu()
{
   switch (fd_dev_gen(&dev_id)) {
   case 6:
      init_rnn("a6xx");
      break;
   case 7:
      init_rnn("a7xx");
      break;
   default:
      errx(-1, "unsupported gpu: %u", dev_id.gpu_id);
   }
}

static int
handle_file(const char *filename)
{
   struct io *io;
   int submit = 0;
   bool needs_reset = false;
   struct rd_parsed_section ps = {0};

   if (!strcmp(filename, "-"))
      io = io_openfd(0);
   else
      io = io_open(filename);

   if (!io) {
      fprintf(stderr, "could not open: %s\n", filename);
      return -1;
   }

   mem_ctx = ralloc_context(NULL);
   _mesa_set_init(&dumped_shaders, mem_ctx, u64_hash, u64_compare);

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
         print_commands(hostptr(gpuaddr), sizedwords);
         needs_reset = true;
         submit++;
         break;
      }
      case RD_GPU_ID: {
         dev_id.gpu_id = parse_gpu_id(ps.buf);
         if (fd_dev_info(&dev_id))
            init_gpu();
         break;
      }
      case RD_CHIP_ID: {
         dev_id.chip_id = parse_chip_id(ps.buf);
         if (fd_dev_info(&dev_id))
            init_gpu();
         break;
      }
      default:
         break;
      }
   }

   ralloc_free(mem_ctx);
   io_close(io);
   fflush(stdout);

   if (ps.ret < 0) {
      fprintf(stderr, "corrupt file\n");
   }
   return 0;
}
