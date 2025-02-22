/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/os_file.h"
#include "util/ralloc.h"

#include "tar.h"

typedef struct content {
   const char    *name;
   const uint8_t *start;
   const uint8_t *end;
} content;

typedef struct object {
   const char *prefix;
   const char *name;

   int      versions_count;
   content *versions;

   struct mesa_archive *ma;
} object;

typedef struct mesa_archive {
   const char *filename;

   uint8_t *contents;
   unsigned contents_size;

   int     objects_count;
   object *objects;

   const char *info;
} mesa_archive;

typedef struct context {
   char **args;
   int    args_count;

   mesa_archive **archives;
   int            archives_count;
} context;

const char DEFAULT_DIFF_COMMAND[] = "git diff --no-index --color-words -- %s %s | tail -n +5";
const char DEFAULT_SPIRV_DIS_COMMAND[] = "spirv-dis --color %s";

static void PRINTFLIKE(1, 2)
failf(const char *fmt, ...)
{
   fflush(stdout);
   va_list args;
   va_start(args, fmt);
   vfprintf(stderr, fmt, args);
   va_end(args);
   exit(1);
}

static bool
startswith(const char *prefix, const char *s)
{
   return !strncmp(prefix, s, strlen(prefix));
}

static bool
endswith(const char *suffix, const char *s)
{
   const int suffix_len = strlen(suffix);
   const int len = strlen(s);
   if (suffix_len > len)
      return false;
   return !memcmp(suffix, s + (len - suffix_len), suffix_len);
}

typedef struct {
   FILE *f;
   const char *path;
} temp_file;

static temp_file
make_temp_file(void *mem_ctx)
{
   char path[] = "/tmp/fileXXXXXX";

   int fd = mkstemp(path);
   if (fd == -1)
      failf("mda: failed creating temporary file: %s", strerror(errno));

   FILE *f = fdopen(fd, "w");
   if (!f)
      failf("mda: failed creating temporary file: %s", strerror(errno));

   temp_file r = {0};
   r.f = f;
   r.path = ralloc_strdup(mem_ctx, path);
   return r;
}

static void
diff(const uint8_t *a_start, const uint8_t *a_end,
     const uint8_t *b_start, const uint8_t *b_end)
{
   void *mem_ctx = ralloc_context(NULL);

   temp_file a = make_temp_file(mem_ctx);
   temp_file b = make_temp_file(mem_ctx);

   fwrite(a_start, a_end - a_start, 1, a.f);
   fwrite(b_start, b_end - b_start, 1, b.f);

   fclose(a.f);
   fclose(b.f);

   static const char *diff_cmd = NULL;
   if (!diff_cmd) {
      diff_cmd = getenv("MDA_DIFF_COMMAND");
      if (!diff_cmd)
         diff_cmd = DEFAULT_DIFF_COMMAND;
   }

   char *cmd = ralloc_asprintf(mem_ctx, diff_cmd, a.path, b.path);

   /* Make sure everything printed so far is flushed before the diff
    * subprocess print things.
    */
   fflush(stdout);

   system(cmd);

   unlink(a.path);
   unlink(b.path);
   ralloc_free(mem_ctx);
}

static char *
ralloc_str_from_range(void *mem_ctx, const uint8_t *start, const uint8_t *end)
{
   assert(start);
   assert(end);
   assert(start <= end);
   return ralloc_strndup(mem_ctx, (const char *)start, end - start);
}

static mesa_archive *
parse_mesa_archive(void *mem_ctx, const char *filename)
{
   size_t size = 0;
   uint8_t *contents = (uint8_t *)os_read_file(filename, &size);
   if (!contents) {
      fprintf(stderr, "mda: error reading file %s: %s\n", filename, strerror(errno));
      return NULL;
   }

   mesa_archive *ma = rzalloc(mem_ctx, mesa_archive);
   ma->filename = ralloc_strdup(ma, filename);
   ma->contents = ralloc_memdup(ma, (const char *)contents, size);
   ma->contents_size = size;
   free(contents);

   tar_reader tr = {0};
   tar_reader_init_from_bytes(&tr, ma->contents, size);

   object *cur_object = NULL;

   tar_reader_entry entry = {0};

   {
      if (!tar_reader_next(&tr, &entry) || entry.error) {
         fprintf(stderr, "mda: wrong archive, missing mesa.txt\n");
         return NULL;
      }

      const char *mesa_txt = ralloc_str_from_range(ma, entry.name_start, entry.name_end);
      if (strcmp(mesa_txt, "mesa.txt")) {
         fprintf(stderr, "mda: wrong archive, missing mesa.txt\n");
         return NULL;
      }

      ma->info = ralloc_str_from_range(ma, entry.contents_start, entry.contents_end);
   }

   while (tar_reader_next(&tr, &entry)) {
      char *prefix = ralloc_str_from_range(ma, entry.prefix_start, entry.prefix_end);
      char *name   = ralloc_str_from_range(ma, entry.name_start,   entry.name_end);

      char *slash = strchr(name, '/');
      char *version_name;
      if (slash) {
         assert(slash < name + strlen(name) - 1);
         *slash = 0;
         version_name = slash + 1;
      } else {
         version_name = "";
      }

      assert(strlen(prefix) > 4);
      assert(startswith("mda/", prefix));
      prefix = &prefix[4];

      if (!cur_object || strcmp(prefix, cur_object->prefix) || strcmp(name, cur_object->name)) {
         ma->objects = rerzalloc(ma, ma->objects, object, ma->objects_count, ma->objects_count + 1);
         cur_object = &ma->objects[ma->objects_count++];
         cur_object->prefix = prefix;
         cur_object->name = name;
         cur_object->ma = ma;
      }

      cur_object->versions = rerzalloc(ma, cur_object->versions, content,
                                    cur_object->versions_count, cur_object->versions_count + 1);
      int s = cur_object->versions_count++;

      cur_object->versions[s].name  = version_name;
      cur_object->versions[s].start = entry.contents_start;
      cur_object->versions[s].end   = entry.contents_end;
   }

   return ma;
}

static void
print_repeated(char c, int count)
{
   for (; count > 0; count--)
      putchar(c);
}

typedef struct {
   const char *file;
   const char *prefix;
   const char *name;
} pattern_parts;

static pattern_parts
parse_pattern(context *ctx, const char *input)
{
   pattern_parts parts = {0};

   char *pat = ralloc_strdup(ctx, input);

   char *at_separator = strchr(pat, '@');
   char *slash_separator = strchr(pat, '/');
   if (at_separator && slash_separator)
      assert(at_separator < slash_separator);

   if (at_separator) {
      parts.file = pat;
      *at_separator = '\0';
      pat = at_separator + 1;
   } else {
      parts.file = "";
   }

   if (slash_separator) {
      parts.prefix = pat;
      *slash_separator = '\0';
      pat = slash_separator + 1;
   } else {
      parts.prefix = "";
   }

   parts.name = pat;

   return parts;
}

static object *
find_object(context *ctx, const char *pattern)
{
   if (!pattern || !*pattern)
      pattern = "";

   pattern_parts parts = parse_pattern(ctx, pattern);
   object **matches = NULL;
   int count = 0;

   void *tmp_ctx = ralloc_context(ctx);

   for (int i = 0; i < ctx->archives_count; i++) {
      mesa_archive *ma = ctx->archives[i];

      for (object *obj = ma->objects; obj < ma->objects + ma->objects_count; obj++) {
         const bool matched =
            startswith(parts.file, ma->filename) &&
            startswith(parts.prefix, obj->prefix) &&
            strstr(obj->name, parts.name) != NULL;

         if (matched) {
            matches = rerzalloc(tmp_ctx, matches, object *, count, count+1);
            matches[count++] = obj;
         }
      }
   }

   object *match = NULL;
   if (count == 1) {
      match = matches[0];
   } else if (count == 0) {
      fprintf(stderr, "mda: couldn't find object for pattern: %s\n", pattern);
   } else {
      fprintf(stderr, "error: multiple matches for pattern: %s\n", pattern);

      bool needs_file = false;
      for (int i = 1; i < count; i++) {
         if (matches[i-1]->ma != matches[i]->ma) {
            needs_file = true;
            break;
         }
      }

      for (int i = 0; i < count; i++) {
         const object *obj = matches[i];
         if (needs_file) {
            fprintf(stderr, "    %s@%s/%s\n", obj->ma->filename, obj->prefix, obj->name);
         } else {
            fprintf(stderr, "    %s/%s\n", obj->prefix, obj->name);
         }
      }
   }

   ralloc_free(tmp_ctx);
   return match;
}

static int
cmd_info(context *ctx)
{
   for (int i = 0; i < ctx->archives_count; i++) {
      if (i > 0) {
         printf("\n");
      }

      mesa_archive *ma = ctx->archives[i];
      printf("# From %s\n\n", ma->filename);

      printf("%s\n", ma->info);

      const char *cur_name = "";

      for (object *obj = ma->objects; obj < ma->objects + ma->objects_count; obj++) {
         if (strcmp(cur_name, obj->prefix)) {
            printf("  %s\n", obj->prefix);
            cur_name = obj->prefix;
         }
      }
   }

   return 0;
}

static int
cmd_list(context *ctx)
{
   for (int i = 0; i < ctx->archives_count; i++) {
      if (i > 0) {
         printf("\n");
      }

      mesa_archive *ma = ctx->archives[i];
      printf("# From %s\n", ma->filename);

      const char *cur_name = "";

      for (object *obj = ma->objects; obj < ma->objects + ma->objects_count; obj++) {
         if (strcmp(cur_name, obj->prefix)) {
            printf("\n  %s/\n", obj->prefix);
            cur_name = obj->prefix;
         }
         printf("    %s", obj->name);
         if (obj->versions_count > 1)
            printf(" (%d versions)", obj->versions_count);
         printf("\n");
      }
   }

   return 0;
}

static int
cmd_logsum(context *ctx)
{
   if (ctx->args_count == 0) {
      fprintf(stderr, "mda: need to pass an object to log\n");
      return 1;
   }

   const char *pattern = ctx->args[0];

   object *obj = find_object(ctx, pattern);
   if (!obj)
      return 1;

   printf("### %s/%s\n", obj->prefix, obj->name);

   for (int i = 0; i < obj->versions_count; i++) {
      const content *c = &obj->versions[i];
      printf("%s (%d)\n", c->name, i);
   }

   printf("\n");

   return 0;
}

static int
cmd_diff(context *ctx)
{
   if (ctx->args_count != 2 && ctx->args_count != 3) {
      fprintf(stderr, "mda: invalid arguments\n");
      return 1;
   }

   const content *content_a = NULL;
   const content *content_b = NULL;

   if (ctx->args_count == 2) {
      object *a = find_object(ctx, ctx->args[0]);
      object *b = find_object(ctx, ctx->args[1]);

      if (!a || !b)
         return 1;

      content_a = &a->versions[a->versions_count - 1];
      content_b = &b->versions[b->versions_count - 1];

      int m = 0;
      int n = 0;
      if (a->ma == b->ma) {
         m = printf("# From %s\n", a->ma->filename);
         n = printf("# %s/%s and %s/%s\n", a->prefix, a->name,
                                           b->prefix, b->name);
      } else {
         m = printf("# %s/%s from %s and\n", a->prefix, a->name, a->ma->filename);
         n = printf("# %s/%s from %s\n",     b->prefix, b->name, b->ma->filename);
      }
      print_repeated('#', MAX2(m, n) - 1);
      printf("\n");

   } else {
      assert(ctx->args_count == 3);

      const char *pattern = ctx->args[0];

      const int a = atoi(ctx->args[1]);
      const int b = atoi(ctx->args[2]);

      object *obj = find_object(ctx, pattern);
      if (!obj)
         return 1;

      if (a < 0 || a >= obj->versions_count) {
         fprintf(stderr, "mda: invalid version number: %s\n", ctx->args[0]);
         return 1;
      }

      if (b < 0 || b >= obj->versions_count) {
         fprintf(stderr, "mda: invalid version number: %s\n", ctx->args[1]);
         return 1;
      }

      content_a = &obj->versions[a];
      content_b = &obj->versions[b];

      int m = printf("# From %s/%s in %s\n", obj->prefix, obj->name, obj->ma->filename);
      int n = printf("# %s (%d) -> %s (%d)\n", content_a->name, a, content_b->name, b);
      print_repeated('#', MAX2(m, n) - 1);
      printf("\n");
   }

   diff(content_a->start, content_a->end,
        content_b->start, content_b->end);
   printf("\n");

   return 0;
}

static int
cmd_log(context *ctx)
{
   const char *pattern = ctx->args[0];

   object *obj = find_object(ctx, pattern);
   if (!obj)
      return 1;

   for (int i = 1; i < obj->versions_count; i++) {
      const content *new = &obj->versions[i];
      const content *old = &obj->versions[i-1];

      int n = printf("# %s (%d) -> %s (%d)\n", old->name, i-1, new->name, i);
      print_repeated('#', n-1);
      printf("\n");

      diff(old->start, old->end,
           new->start, new->end);
      printf("\n");
   }

   printf("\n");
   return 0;
}

static int
print_disassembled_spirv(object *sh)
{
   assert(!strcmp(sh->name, "SPIRV"));
   assert(sh->versions_count == 1);

   content *c = &sh->versions[0];

   FILE *f = tmpfile();
   fwrite(c->start, c->end - c->start, 1, f);
   fflush(f);

   int fd = fileno(f);

   static const char *spirv_dis_cmd = NULL;
   if (!spirv_dis_cmd) {
      spirv_dis_cmd = getenv("MDA_SPIRV_DIS_COMMAND");
      if (!spirv_dis_cmd)
         spirv_dis_cmd = DEFAULT_SPIRV_DIS_COMMAND;
   }

   char path[128] = {0};
   snprintf(path, sizeof(path)-2, "<(cat /proc/self/fd/%d)", fd);

   char cmd[1024] = {0};
   snprintf(cmd, sizeof(cmd)-2, spirv_dis_cmd, path);
   fflush(stdout);
   system(cmd);
   fclose(f);

   return 0;
}

static int
cmd_print(context *ctx)
{
   if (ctx->args_count == 0) {
      fprintf(stderr, "mda: need to pass an object to print\n");
      return 1;
   }

   const char *pattern = ctx->args[0];

   object *obj = find_object(ctx, pattern);
   if (!obj || !obj->versions_count)
      return 1;

   if (!strcmp(obj->name, "SPIRV")) {
      return print_disassembled_spirv(obj);
   }

   printf("### %s/%s\n", obj->prefix, obj->name);

   int version;
   if (ctx->args_count > 1) {
      version = atoi(ctx->args[1]);
      if (version < 0 || version >= obj->versions_count) {
         fprintf(stderr, "mda: invalid version number: %s\n", ctx->args[1]);
         return 1;
      }
   } else {
      version = obj->versions_count - 1;
   }

   const content *c = &obj->versions[version];

   if (obj->versions_count > 1) {
      int n = printf("# %s (%d)\n", c->name, version);
      print_repeated('#', n-1);
      printf("\n");
   }

   fwrite(c->start, c->end - c->start, 1, stdout);
   printf("\n");

   return 0;
}

static int
cmd_printraw(context *ctx)
{
   if (ctx->args_count == 0) {
      fprintf(stderr, "mda: need to pass an object to print\n");
      return 1;
   }

   const char *pattern = ctx->args[0];

   object *obj = find_object(ctx, pattern);
   if (!obj || !obj->versions_count)
      return 1;

   int version;
   if (ctx->args_count > 1) {
      version = atoi(ctx->args[1]);
      if (version < 0 || version >= obj->versions_count) {
         fprintf(stderr, "mda: invalid version number: %s\n", ctx->args[1]);
         return 1;
      }
   } else {
      version = obj->versions_count - 1;
   }

   const content *c = &obj->versions[version];
   fwrite(c->start, c->end - c->start, 1, stdout);

   return 0;
}

static int
cmd_logfull(context *ctx)
{
   if (ctx->args_count == 0) {
      fprintf(stderr, "mda: need to pass an object to log\n");
      return 1;
   }

   const char *pattern = ctx->args[0];

   object *obj = find_object(ctx, pattern);
   if (!obj)
      return 1;

   if (obj->versions_count == 1)
      return cmd_print(ctx);

   printf("### %s/%s\n", obj->prefix, obj->name);

   for (int i = 0; i < obj->versions_count; i++) {
      const content *c = &obj->versions[i];

      int n = printf("# %s (%d)\n", c->name, i);
      print_repeated('#', n-1);
      printf("\n");

      fwrite(c->start, c->end - c->start, 1, stdout);
      printf("\n");
   }

   printf("\n");

   return 0;
}

static int
cmd_help()
{
   printf("mda [[-f FILENAME]...] CMD [ARGS...]\n"
          "\n"
          "Reads *.mda.tar files generated by Mesa drivers, these\n"
          "files contain debugging information about a pipeline or\n"
          "a single shader stage.\n"
          "\n"
          "Without command, all the objects are listed, an object can\n"
          "be a particular internal shader form or other metadata.\n"
          "Objects are identified by matching a PATTERN in the form\n"
          "\n"
          "  file@prefix/name\n"
          "\n"
          "The two first parts (file and prefix) are optional, so\n"
          "'CS', 'before@CS', '123/CS' and 'before@123/CS' are all valid.\n"
          "\n"
          "Objects may have multiple versions, e.g. multiple versions\n"
          "of a shader stage generated during optimization.  When not\n"
          "specified, commands use the last version in the archive.\n"
          "Versions are identified by a number between parenthesis in\n"
          "the `log` commands output.\n"
          "\n"
          "By default all *.mda.tar files are read.  To specify which files\n"
          "read use one or more `-f FILENAME` flags before the command.\n"
          "\n"
          "COMMANDS\n"
          "\n"
          "    list                           list all objects\n"
          "    print       PATTERN [V]        formatted print version V (or last) of an object\n"
          "    printraw    PATTERN [V]        raw dump of version V (or last) of object\n"
          "    log         PATTERN            print changes between versions\n"
          "    logfull     PATTERN            print full contents of all versions\n"
          "    logsum      PATTERN            print the names of the versions\n"
          "    diff        PATTERN V1 V2      compare two versions of an object\n"
          "    diff        PATTERN1 PATTERN2  compare two objects\n"
          "    info                           print metadata about the archive\n"
          "\n"
          "The diff program used by mda can be configured by setting\n"
          "the MDA_DIFF_COMMAND environment variable.  By default it\n"
          "uses git-diff -- that works even without a git repository:\n"
          "\n"
          "    MDA_DIFF_COMMAND=\"%s\"\n"
          "\n"
          "When showing SPIR-V files, a disassembler program is used.\n"
          "It can be configured by setting the MDA_SPIRV_DIS_COMMAND\n"
          "environment variable.  By default it uses\n"
          "\n"
          "    MDA_SPIRV_DIS_COMMAND=\"%s\"\n"
          "\n", DEFAULT_DIFF_COMMAND, DEFAULT_SPIRV_DIS_COMMAND);
   return 0;
}

static bool
is_help(const char *arg)
{
   return !strcmp(arg, "help") ||
          !strcmp(arg, "--help") ||
          !strcmp(arg, "-help") ||
          !strcmp(arg, "-h");
}

struct command {
   const char *name;
   int (*func)(context *ctx);
};

static bool
load_archive(context *ctx, const char *filename)
{
   struct mesa_archive *ma = parse_mesa_archive(ctx, filename);
   if (!ma)
      return false;

   ctx->archives = rerzalloc(ctx, ctx->archives, mesa_archive *, ctx->archives_count,
                             ctx->archives_count + 1);
   ctx->archives[ctx->archives_count] = ma;
   ctx->archives_count++;
   return true;
}

int
main(int argc, char *argv[])
{
   if (argc >= 2 && is_help(argv[1])) {
      cmd_help();
      return 0;
   }

   context *ctx = rzalloc(NULL, context);

   int cur_arg = 1;

   while (cur_arg < argc && !strcmp(argv[cur_arg], "-f")) {
      if (argc == cur_arg + 1)
         failf("mda: missing filename after -f flag\n");

      const char *filename = argv[cur_arg + 1];
      cur_arg += 2;

      for (int i = 0; i < ctx->archives_count; i++) {
         mesa_archive *ma = ctx->archives[i];

         /* Don't load duplicate files from command line. */
         if (!strcmp(filename, ma->filename)) {
            filename = NULL;
            break;
         }
      }

      if (filename && !load_archive(ctx, filename))
         failf("mda: failed to parse file: %s\n", filename);
   }

   if (ctx->archives_count == 0) {
      /* Load all mda files in the current directory. */
      DIR *d;
      struct dirent *dir;
      d = opendir(".");
      if (!d)
         failf("mda: couldn't find *.mda.tar files in current directory: %s\n", strerror(errno));

      while ((dir = readdir(d)) != NULL) {
         if (endswith(".mda.tar", dir->d_name)) {
            if (!load_archive(ctx, dir->d_name)) {
               fprintf(stderr, "mda: ignoring file after parsing failure: %s\n", dir->d_name);
               continue;
            }
         }
      }
      closedir(d);

      if (ctx->archives_count == 0)
         failf("Couldn't load any *.mda.tar files in the current directory\n");
   }

   const char *cmd_name = cur_arg < argc ? argv[cur_arg++] : "list";

   ctx->args_count = argc - cur_arg;
   ctx->args = rzalloc_array(ctx, char *, argc - cur_arg + 1);
   for (int i = 0; i < ctx->args_count; i++) {
      ctx->args[i] = ralloc_strdup(ctx, argv[cur_arg + i]);
   }

   static const struct command cmds[] = {
      { "diff",       cmd_diff },
      { "info",       cmd_info },
      { "list",       cmd_list },
      { "log",        cmd_log },
      { "logfull",    cmd_logfull },
      { "logsum",     cmd_logsum },
      { "print",      cmd_print },
      { "printraw",   cmd_printraw },
   };

   const struct command *cmd = NULL;
   for (const struct command *c = cmds; c < cmds + ARRAY_SIZE(cmds); c++) {
      if (!strcmp(c->name, cmd_name)) {
         cmd = c;
         break;
      }
   }

   if (!cmd) {
      fprintf(stderr, "mda: unknown command '%s'\n", cmd_name);
      cmd_help();
      return 1;
   }

   int r = cmd->func(ctx);
   ralloc_free(ctx);

   return r;
}
