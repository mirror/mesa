/*
 * Copyright © 2024 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <gtest/gtest.h>
#include "tar.h"

TEST(Tar, RoundtripSmallFile)
{
   FILE *f = tmpfile();
   const char *test = "TEST TEST TEST";

   {
      tar_writer tw;
      tar_writer_init(&tw, f);

      tar_writer_start_file(&tw, "test");

      fwrite(test, strlen(test), 1, f);

      tar_writer_finish_file(&tw);
   }

   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   ASSERT_TRUE(size > 0);
   ASSERT_TRUE(size % 512 == 0);
   uint8_t *contents = new uint8_t[size];

   fseek(f, 0, SEEK_SET);
   fread(contents, size, 1, f);
   fclose(f);

   {
      tar_reader ar;
      tar_reader_init_from_bytes(&ar, contents, size);

      tar_reader_entry entry;

      bool first_read = tar_reader_next(&ar, &entry);
      ASSERT_TRUE(first_read);
      ASSERT_FALSE(entry.error);

      ASSERT_EQ(entry.name_end - entry.name_start, 4);
      ASSERT_TRUE(memcmp(entry.name_start, "test", 4) == 0);

      ASSERT_EQ(entry.contents_end - entry.contents_start, strlen(test));
      ASSERT_TRUE(memcmp(entry.contents_start, test, strlen(test)) == 0);

      bool second_read = tar_reader_next(&ar, &entry);
      ASSERT_FALSE(second_read);
      ASSERT_FALSE(entry.error);
   }

   delete[] contents;
}

TEST(Tar, RoundtripContentsWithRecordSize)
{
   FILE *f = tmpfile();
   uint8_t test[512];

   for (unsigned i = 0; i < sizeof(test); i++) {
      test[i] = 'A' + (i % 26);
   }

   {
      tar_writer tw;
      tar_writer_init(&tw, f);
      tar_writer_file_from_bytes(&tw, "test", test, sizeof(test));
      ASSERT_FALSE(tw.error);
   }

   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   ASSERT_TRUE(size > 0);
   ASSERT_TRUE(size % 512 == 0);
   uint8_t *contents = new uint8_t[size];

   fseek(f, 0, SEEK_SET);
   fread(contents, size, 1, f);
   fclose(f);

   {
      tar_reader ar;
      tar_reader_init_from_bytes(&ar, contents, size);
      ASSERT_FALSE(ar.error);

      tar_reader_entry entry;

      bool first_read = tar_reader_next(&ar, &entry);
      ASSERT_TRUE(first_read);
      ASSERT_FALSE(entry.error);

      ASSERT_EQ(entry.name_end - entry.name_start, 4);
      ASSERT_TRUE(memcmp(entry.name_start, "test", 4) == 0);

      ASSERT_EQ(entry.contents_end - entry.contents_start, sizeof(test));
      ASSERT_TRUE(memcmp(entry.contents_start, test, sizeof(test)) == 0);

      bool second_read = tar_reader_next(&ar, &entry);
      ASSERT_FALSE(second_read);
      ASSERT_FALSE(entry.error);
   }

   delete[] contents;
}
