/*
 * Copyright (C) 2017, British Broadcasting Corporation
 * All Rights Reserved.
 *
 * Author: Philip de Nier
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the British Broadcasting Corporation nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <assert.h>
#include <ctype.h>


#define CHK(cmd)                                                                    \
    do {                                                                            \
        if (!(cmd)) {                                                               \
            fprintf(stderr, "'%s' check failed at line %d\n", #cmd, __LINE__);      \
            return 0;                                                               \
        }                                                                           \
    } while (0)


typedef struct
{
    int skip_frame_data;
    int show_props;
    int color_prim_update;
    int transfer_ch_update;
    int matrix_coeff_update;

    FILE *file;
    int eof;

    uint8_t current_byte;
    int next_bit;
    uint64_t value;
} ParseContext;



static int read_next_byte(ParseContext *context)
{
    int c;

    c = fgetc(context->file);
    if (c == EOF) {
        if (feof(context->file))
            context->eof = 1;
        else
            fprintf(stderr, "File read error: %s\n", strerror(errno));
        return 0;
    }

    context->next_bit = 7;
    context->current_byte = (uint8_t)c;

    return 1;
}

static int64_t get_file_pos(ParseContext *context)
{
    return ftello(context->file) - (context->next_bit >= 0 ? 1 : 0);
}

static int seek_to_offset(ParseContext *context, int64_t offset)
{
    if (fseeko(context->file, offset, SEEK_SET) < 0)
        return 0;

    context->next_bit = -1;

    return 1;
}

static int update_file(ParseContext *context, const uint8_t *data, size_t size)
{
    if (fwrite(data, size, 1, context->file) != 1) {
        fprintf(stderr, "Failed to update file: %s\n", strerror(errno));
        return 0;
    }

    context->next_bit = -1;

    return 1;
}


static int have_byte(ParseContext *context)
{
    return context->next_bit >= 0 || read_next_byte(context);
}

static int skip_bytes_align(ParseContext *context, int64_t count)
{
    int64_t offset = count;

    if (context->next_bit >= 0)
        offset--;

    if (offset <= 0) {
        offset = 0;
    } else if (fseeko(context->file, offset, SEEK_CUR) < 0) {
        fprintf(stderr, "Seek error: %s\n", strerror(errno));
        return 0;
    }

    context->next_bit = -1;

    return 1;
}

static int read_bits(ParseContext *context, int n)
{
    int i;

    assert(n <= 64);

    context->value = 0;
    for (i = 0; i < n; i++) {
        if (context->next_bit < 0 && !read_next_byte(context))
            return 0;
        context->value <<= 1;
        context->value |= (context->current_byte >> context->next_bit) & 0x1;
        context->next_bit--;
    }

    return 1;
}

#define f(a)  CHK(read_bits(context, a))
#define u(a)  CHK(read_bits(context, a))

static int frame_header(ParseContext *context)
{
    static const int64_t COLOR_PRIMARIES_OFFSET = 14;
    uint8_t color_primaries;
    uint8_t transfer_characteristic;
    uint8_t matrix_coefficients;
    int64_t file_pos = get_file_pos(context);

    CHK(seek_to_offset(context, file_pos + COLOR_PRIMARIES_OFFSET));

    u(8); color_primaries         = (uint8_t)context->value;
    u(8); transfer_characteristic = (uint8_t)context->value;
    u(8); matrix_coefficients     = (uint8_t)context->value;

    if (context->show_props) {
        printf("First frame properties:\n");
        printf("  color_primaries         : %u\n", color_primaries);
        printf("  transfer_characteristic : %u\n", transfer_characteristic);
        printf("  matrix_coefficients     : %u\n", matrix_coefficients);
    } else {
        uint8_t update[3];
        update[0] = (context->color_prim_update >= 0   ? context->color_prim_update   : color_primaries); 
        update[1] = (context->transfer_ch_update >= 0  ? context->transfer_ch_update  : transfer_characteristic); 
        update[2] = (context->matrix_coeff_update >= 0 ? context->matrix_coeff_update : matrix_coefficients); 
        CHK(seek_to_offset(context, file_pos + COLOR_PRIMARIES_OFFSET));
        CHK(update_file(context, update, sizeof(update)));
    }

    return 1;
}

static int frame(ParseContext *context)
{
    static const uint32_t RDD36_FRAME_ID = 0x69637066; // 'icpf'
    uint32_t frame_size;
    int64_t file_pos = get_file_pos(context);
    int64_t skip_size;

    u(32); frame_size = (uint32_t)context->value;
    f(32);
    CHK(context->value == RDD36_FRAME_ID);
    CHK(frame_header(context));

    if (context->skip_frame_data) {
        skip_size = frame_size - (get_file_pos(context) - file_pos);
        if (skip_size > 0)
            CHK(skip_bytes_align(context, skip_size));
    }

    return 1;
}

static int read_next_frame_offset(FILE *offsets_file, int64_t *offset_out)
{
    char line[1024];
    size_t i;

    while (1) {
        if (!fgets(line, sizeof(line), offsets_file))
            return 0;
        for (i = 0; i < sizeof(line); i++) {
            if ((line[i] >= '0' && line[i] <= '9') || !line[i])
                break;
        }
        if (i < sizeof(line) && line[i]) {
            int64_t offset;
            if (sscanf(&line[i], "%" PRId64, &offset) == 1 && offset >= 0) {
                *offset_out = offset;
                return 1;
            }
        }
    }

    return 0;
}

static void print_usage(const char *cmd)
{
    fprintf(stderr, "Usage: %s [options] <filename>\n", cmd);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h | --help    Show help and exit\n");
    fprintf(stderr, "  -s             Show properties in the first frame and exit\n");
    fprintf(stderr, "  -p <val>       Modify the 'color_primaries' property to <val>\n");
    fprintf(stderr, "  -t <val>       Modify the 'transfer_characteristic' property to <val>\n");
    fprintf(stderr, "  -m <val>       Modify the 'matrix_coefficients' property to <val>\n");
    fprintf(stderr, "  -o <file>      Text file containing decimal file offsets for each frame separated by a newline\n");
    fprintf(stderr, "                     E.g. using ffprobe to extract offsets from a Quicktime file:\n");
    fprintf(stderr, "                     'ffprobe -show_packets -select_streams v:0 example.mov | grep pos >offsets.txt'\n");
}

int main(int argc, const char **argv)
{
    const char *offsets_filename = NULL;
    const char *filename;
    int cmdln_index;
    ParseContext context;
    FILE *offsets_file = NULL;
    int result = 0;

    memset(&context, 0, sizeof(context));
    context.next_bit = -1;
    context.transfer_ch_update = -1;
    context.matrix_coeff_update = -1;
    context.color_prim_update = -1;

    if (argc <= 1) {
        print_usage(argv[0]);
        return 0;
    }

    for (cmdln_index = 1; cmdln_index < argc; cmdln_index++) {
        if (strcmp(argv[cmdln_index], "-h") == 0 ||
            strcmp(argv[cmdln_index], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[cmdln_index], "-s") == 0)
        {
            context.show_props = 1;
        }
        else if (strcmp(argv[cmdln_index], "-t") == 0)
        {
            if (cmdln_index + 1 >= argc)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Missing argument for option '%s'\n", argv[cmdln_index]);
                return 1;
            }
            if (sscanf(argv[cmdln_index + 1], "%d", &context.transfer_ch_update) != 1 ||
                context.transfer_ch_update < 0 || context.transfer_ch_update > 0xff)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Invalid value '%s' for option '%s'\n", argv[cmdln_index + 1], argv[cmdln_index]);
                return 1;
            }
            cmdln_index++;
        }
        else if (strcmp(argv[cmdln_index], "-m") == 0)
        {
            if (cmdln_index + 1 >= argc)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Missing argument for option '%s'\n", argv[cmdln_index]);
                return 1;
            }
            if (sscanf(argv[cmdln_index + 1], "%d", &context.matrix_coeff_update) != 1 ||
                context.matrix_coeff_update < 0 || context.matrix_coeff_update > 0xff)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Invalid value '%s' for option '%s'\n", argv[cmdln_index + 1], argv[cmdln_index]);
                return 1;
            }
            cmdln_index++;
        }
        else if (strcmp(argv[cmdln_index], "-p") == 0)
        {
            if (cmdln_index + 1 >= argc)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Missing argument for option '%s'\n", argv[cmdln_index]);
                return 1;
            }
            if (sscanf(argv[cmdln_index + 1], "%d", &context.color_prim_update) != 1 ||
                context.color_prim_update < 0 || context.color_prim_update > 0xff)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Invalid value '%s' for option '%s'\n", argv[cmdln_index + 1], argv[cmdln_index]);
                return 1;
            }
            cmdln_index++;
        }
        else if (strcmp(argv[cmdln_index], "-o") == 0)
        {
            if (cmdln_index + 1 >= argc)
            {
                print_usage(argv[0]);
                fprintf(stderr, "Missing argument for option '%s'\n", argv[cmdln_index]);
                return 1;
            }
            offsets_filename = argv[cmdln_index + 1];
            cmdln_index++;
        }
        else
        {
            break;
        }
    }

    if (cmdln_index + 1 < argc) {
        print_usage(argv[0]);
        fprintf(stderr, "Unknown option '%s'\n", argv[cmdln_index]);
        return 1;
    }
    if (cmdln_index >= argc) {
        print_usage(argv[0]);
        fprintf(stderr, "Missing <filename>\n");
        return 1;
    }

    filename = argv[cmdln_index];
    if (context.transfer_ch_update < 0 && context.matrix_coeff_update < 0 && context.color_prim_update < 0)
      context.show_props = 1;
    if (!offsets_filename)
      context.skip_frame_data = 1;


    if (context.show_props)
      context.file = fopen(filename, "rb");
    else
      context.file = fopen(filename, "r+b");
    if (!context.file) {
        fprintf(stderr, "Failed to open input file '%s': %s\n", filename, strerror(errno));
        return 1;
    }

    if (offsets_filename) {
        offsets_file = fopen(offsets_filename, "rb");
        if (!offsets_file) {
            fprintf(stderr, "Failed to open offsets file '%s': %s\n", offsets_filename, strerror(errno));
            return 1;
        }
    }

    while (1) {
        if (offsets_file) {
            int64_t offset;
            if (!read_next_frame_offset(offsets_file, &offset) || !seek_to_offset(&context, offset))
                break;
        } 
        if (!have_byte(&context))
            break;
        if (!frame(&context)) {
            result = 1;
            break;
        }
        if (context.show_props)
          break;
    }

    if (context.file)
        fclose(context.file);
    if (offsets_file)
        fclose(offsets_file);


    return result;
}
