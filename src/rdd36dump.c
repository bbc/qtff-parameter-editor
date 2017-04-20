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
#include <limits.h>


#define PRINT_UINT(name)        printf("%*c " name ": %"      PRIu64 "\n", context->indent * 4, ' ', context->value)
#define PRINT_UINT8_HEX(name)   printf("%*c " name ": 0x%02"  PRIx64 "\n", context->indent * 4, ' ', context->value)

#define PRINT_ENUM(name, strings, default_string) \
    print_enum(context, name, strings, ARRAY_SIZE(strings), default_string)

#define ARRAY_SIZE(array)       (sizeof(array) / sizeof((array)[0]))

#define CHK(cmd)                                                                    \
    do {                                                                            \
        if (!(cmd)) {                                                               \
            fprintf(stderr, "'%s' check failed at line %d\n", #cmd, __LINE__);      \
            return 0;                                                               \
        }                                                                           \
    } while (0)


typedef struct
{
    FILE *file;
    int64_t next_read_pos;
    int eof;

    uint8_t current_byte;
    int next_bit;
    uint64_t value;

    int indent;
    int64_t frame_count;

    uint8_t interlace_mode;
    uint8_t picture_header_size;
    uint32_t picture_size;
} ParseContext;


static const char *CHROMA_FORMAT_STRINGS[] =
{
    "Reserved",
    "Reserved",
    "4:2:2",
    "4:4:4",
};

static const char *INTERLACE_MODE_STRINGS[] =
{
    "Progressive frame",
    "Interlaced frame (TFF)",
    "Interlaced frame (BFF)",
    "Reserved",
};

static const char *ASPECT_RATIO_STRINGS[] =
{
    "Unknown/unspecified",
    "Square pixels",
    "4:3",
    "16:9",
};

static const char *FRAME_RATE_STRINGS[] =
{
    "Unknown/unspecified",
    "24/1.001",
    "24",
    "25",
    "30/1.001",
    "30",
    "50",
    "60/1.001",
    "60",
    "100",
    "120/1.001",
    "120",
};

static const char *COLOR_PRIMARY_STRINGS[] =
{
    "Unknown/unspecified",
    "ITU-R BT.709",
    "Unknown/unspecified",
    "Reserved",
    "Reserved",
    "ITU-R BT.601 625",
    "ITU-R BT.601 525",
    "Reserved",
    "Reserved",
    "ITU-R BT.2020",
    "Reserved",
    "DCI P3",
    "P3 D65",
};

static const char *TRANSFER_CHAR_STRINGS[] =
{
    "Unknown/unspecified",
    "ITU-R BT.601/BT.709/BT.2020",
    "Unknown/unspecified",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "SMPTE ST 2084",
    "Reserved",
    "HLG OETF",
};

static const char *MATRIX_COEFF_STRINGS[] =
{
    "Unknown/unspecified",
    "ITU-R BT.709",
    "Unknown/unspecified",
    "Reserved",
    "Reserved",
    "Reserved",
    "ITU-R BT.601",
    "Reserved",
    "Reserved",
    "ITU-R BT.2020",
};

static const char *ALPHA_CHANNEL_TYPE_STRINGS[] =
{
    "Not present",
    "8 bits/sample integral",
    "16 bits/sample integral",
};



static int read_next_byte(ParseContext *context)
{
    int c;

    c = fgetc(context->file);
    if (c == EOF) {
        if (feof(context->file)) {
            context->eof = 1;
        } else {
            fprintf(stderr, "File I/O error: %s\n", strerror(errno));
        }
        return 0;
    }

    context->next_read_pos++;
    context->next_bit = 7;
    context->current_byte = (uint8_t)c;

    return 1;
}

static int64_t get_file_pos(ParseContext *context)
{
    return context->next_read_pos - (context->next_bit >= 0 ? 1 : 0);
}

static int seek_to_offset(ParseContext *context, int64_t offset)
{
    if (fseeko(context->file, offset, SEEK_SET) < 0)
        return 0;

    context->next_bit = -1;
    context->next_read_pos = offset;

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
    context->next_read_pos += offset;

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

#define f(a)    CHK(_f(context, a))
static int _f(ParseContext *context, int num_bits)
{
    return read_bits(context, num_bits);
}

#define u(a)    CHK(_u(context, a))
static int _u(ParseContext *context, int num_bits)
{
    return read_bits(context, num_bits);
}

static void print_structure_start(ParseContext *context, const char *name)
{
    int64_t file_pos = get_file_pos(context);

    printf("%*c %s: pos=%" PRId64 "\n", context->indent * 4, ' ', name, file_pos);
}

static void print_fourcc(ParseContext *context, const char *name)
{
    int i;
    uint32_t value = (uint32_t)context->value;

    printf("%*c %s: 0x%08x (", context->indent * 4, ' ', name, value);

    for (i = 0; i < 4; i++) {
        char c = (char)(value >> (8 * (3 - i)));
        if (isprint(c))
            printf("%c", c);
        else
            printf(".");
    }
    printf(")\n");
}

static void print_enum(ParseContext *context, const char *name, const char **strings, size_t strings_size,
                       const char *default_string)
{
    uint8_t value = (uint8_t)context->value;

    printf("%*c %s: %" PRIu64 , context->indent * 4, ' ', name, context->value);

    if (value < strings_size)
        printf(" (%s)\n", strings[value]);
    else
        printf(" (%s)\n", default_string);
}

static int dump_quantization_matrix(ParseContext *context, const char *name)
{
    int u, v;

    printf("%*c %s:\n", context->indent * 4, ' ', name);

    context->indent++;

    for (v = 0; v < 8; v++) {
        printf("%*c ", context->indent * 4, ' ');
        for (u = 0; u < 8; u++) {
            u(8); printf(" %02x", (uint8_t)context->value);
        }
        printf("\n");
    }

    context->indent--;

    return 1;
}

static int stuffing(ParseContext *context, int64_t size)
{
    print_structure_start(context, "stuffing");

    context->indent++;

    // TODO: report remainder bits?
    printf("%*c size: %" PRIi64 "\n", context->indent * 4, ' ', size);
    CHK(skip_bytes_align(context, size));

    context->indent--;

    return 1;
}

static int picture_header(ParseContext *context)
{
    int64_t file_pos = get_file_pos(context);

    u(5);  PRINT_UINT("picture_header_size");
    context->picture_header_size = (uint8_t)context->value;
    u(3);  PRINT_UINT8_HEX("reserved");
    u(32); PRINT_UINT("picture_size");
    context->picture_size = (uint32_t)context->value;
    u(16); PRINT_UINT("deprecated_number_of_slices");
    u(2);  PRINT_UINT8_HEX("reserved");
    u(2);  PRINT_UINT("log2_desired_slice_size_in_mb");
    u(4);  PRINT_UINT8_HEX("reserved");

    CHK(context->picture_size >= context->picture_header_size);
    int64_t rem_picture_header = context->picture_header_size - (get_file_pos(context) - file_pos);
    CHK(rem_picture_header >= 0);
    // TODO: dump bytes
    CHK(skip_bytes_align(context, rem_picture_header));

    return 1;
}

static int picture(ParseContext *context, int temporal_order)
{
    print_structure_start(context, "picture");

    context->indent++;

    picture_header(context);

    CHK(skip_bytes_align(context, context->picture_size - context->picture_header_size));

    context->indent--;

    return 1;
}

static int frame_header(ParseContext *context)
{
    uint16_t frame_header_size;
    int load_luma_quantization_matrix;
    int load_chroma_quantization_matrix;
    int64_t file_pos = get_file_pos(context);

    printf("%*c frame_header:\n", context->indent * 4, ' ');

    context->indent++;

    u(16); PRINT_UINT("frame_header_size");
    frame_header_size = (uint16_t)context->value;
    u(8);  PRINT_UINT8_HEX("reserved");
    u(8);  PRINT_UINT("bitstream_version");
    f(32); print_fourcc(context, "encoder_identifier");
    u(16); PRINT_UINT("horizontal_size");
    u(16); PRINT_UINT("vertical_size");
    u(2);  PRINT_ENUM("chroma_format", CHROMA_FORMAT_STRINGS, "");
    u(2);  PRINT_UINT8_HEX("reserved");
    u(2);  PRINT_ENUM("interlace_mode", INTERLACE_MODE_STRINGS, "");
    u(2);  PRINT_UINT8_HEX("reserved");
    u(4);  PRINT_ENUM("aspect_ratio_information", ASPECT_RATIO_STRINGS, "Reserved");
    u(4);  PRINT_ENUM("frame_rate_code", FRAME_RATE_STRINGS, "Reserved");
    u(8);  PRINT_ENUM("color_primaries", COLOR_PRIMARY_STRINGS, "Reserved");
    u(8);  PRINT_ENUM("transfer_characteristic", TRANSFER_CHAR_STRINGS, "Reserved");
    u(8);  PRINT_ENUM("matrix_coefficients", MATRIX_COEFF_STRINGS, "Reserved");
    u(4);  PRINT_UINT8_HEX("reserved");
    u(4);  PRINT_ENUM("alpha_channel_type", ALPHA_CHANNEL_TYPE_STRINGS, "Reserved");
    u(14); PRINT_UINT8_HEX("reserved");
    u(1);  PRINT_UINT("load_luma_quantization_matrix");
    load_luma_quantization_matrix = !!context->value;
    u(1);  PRINT_UINT("load_chroma_quantization_matrix");
    load_chroma_quantization_matrix = !!context->value;
    if (load_luma_quantization_matrix)
        CHK(dump_quantization_matrix(context, "luma_quantization_matrix"));
    if (load_chroma_quantization_matrix)
        CHK(dump_quantization_matrix(context, "chroma_quantization_matrix"));

    int64_t rem_frame_header = frame_header_size - (get_file_pos(context) - file_pos);
    CHK(rem_frame_header >= 0);
    // TODO: dump bytes
    CHK(skip_bytes_align(context, rem_frame_header));

    context->indent--;

    return 1;
}

static int frame(ParseContext *context)
{
    static const uint32_t RDD36_FRAME_ID = 0x69637066; // 'icpf'
    uint32_t frame_size;
    int64_t file_pos = get_file_pos(context);
    int64_t stuffing_size;

    printf("frame: num=%" PRId64 ", pos=%" PRId64 "\n", context->frame_count, file_pos);

    context->indent++;

    u(32);  PRINT_UINT("frame_size");
    frame_size = (uint32_t)context->value;
    f(32);  print_fourcc(context, "frame_identifier");
    CHK(context->value == RDD36_FRAME_ID);
    CHK(frame_header(context));
    CHK(picture(context, 1));
    if (context->interlace_mode == 1 || context->interlace_mode == 2)
        CHK(picture(context, 2));
    stuffing_size = frame_size - (get_file_pos(context) - file_pos);
    if (stuffing_size > 0)
        CHK(stuffing(context, stuffing_size));

    context->indent--;

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
    fprintf(stderr, "  -h | --help          Show help and exit\n");
    fprintf(stderr, "  --offsets <file>     Text file containing decimal file offsets for each frame separated by newlines\n");
    fprintf(stderr, "                       E.g. using ffprobe to extract offsets from a Quicktime file:\n");
    fprintf(stderr, "                           'ffprobe -show_packets -select_streams v:0 example.mov | grep pos >offsets.txt'\n");
}

int main(int argc, const char **argv)
{
    const char *offsets_filename = NULL;
    const char *filename;
    int cmdln_index;
    ParseContext context;
    FILE *offsets_file = NULL;
    int result = 0;

    // TODO: options to limit the dump start and count

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
        else if (strcmp(argv[cmdln_index], "--offsets") == 0)
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


    memset(&context, 0, sizeof(context));
    context.next_bit = -1;
    context.file = fopen(filename, "rb");
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
        } else if (!have_byte(&context)) {
            break;
        }
        if (!frame(&context)) {
            result = 1;
            break;
        }
        context.frame_count++;
    }

    if (context.file)
        fclose(context.file);
    if (offsets_file)
        fclose(offsets_file);


    return result;
}