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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <limits.h>

#include <vector>
#include <string>
#include <exception>

using namespace std;



#define MOV_CHECK(cmd) \
    if (!(cmd)) { \
        throw MOVException("%s failed at line %d", #cmd, __LINE__); \
    }

#define ARRAY_SIZE(array)   (sizeof(array) / sizeof((array)[0]))

#define MKTAG(cs) ((((uint32_t)cs[0])<<24)|(((uint32_t)cs[1])<<16)|(((uint32_t)cs[2])<<8)|(((uint32_t)cs[3])))

#define ATOM_INDENT             "    "
#define ATOM_VALUE_INDENT       "  "

#define CURRENT_ATOM            g_atoms.back()
#define HAVE_PREV_ATOM          (g_atoms.size() >= 2)
#define PREV_ATOM               g_atoms[g_atoms.size() - 2]

#define DUMP_FUNC_MAP_SIZE      (sizeof(dump_func_map) / sizeof(DumpFuncMap))



typedef struct
{
    uint64_t size;
    char type[4];
    uint64_t rem_size;
    uint64_t offset;
} MOVAtom;

typedef struct
{
    char type[4];
    void (*dump)();
} DumpFuncMap;


static const uint32_t MHLR_COMPONENT_TYPE     = MKTAG("mhlr");
static const uint32_t VIDE_COMPONENT_SUB_TYPE = MKTAG("vide");
static const uint32_t SOUN_COMPONENT_SUB_TYPE = MKTAG("soun");
static const uint32_t TMCD_COMPONENT_SUB_TYPE = MKTAG("tmcd");


static FILE *g_mov_file = 0;
static vector<MOVAtom> g_atoms;
static uint64_t g_file_offset;
static vector<string> g_meta_keys;
static uint32_t g_movie_timescale;
static uint32_t g_component_type = 0;
static uint32_t g_component_sub_type = 0;
static bool g_qt_brand = true;
static const char *g_avcc_filename = 0;
static FILE *g_avcc_file = 0;
static int mp4_object_desc_level = 0;


class MOVException : public std::exception
{
public:
    MOVException()
    : exception()
    {
    }

    MOVException(const char *format, ...)
    : exception()
    {
        char message[1024];

        va_list varg;
        va_start(varg, format);
#if defined(_MSC_VER)
        int res = _vsnprintf(message, sizeof(message), format, varg);
        if (res == -1 && errno == EINVAL)
            message[0] = 0;
        else
            message[sizeof(message) - 1] = 0;
#else
        if (vsnprintf(message, sizeof(message), format, varg) < 0)
            message[0] = 0;
#endif
        va_end(varg);

        mMessage = message;
    }

    MOVException(const string &message)
    : exception()
    {
        mMessage = message;
    }

    ~MOVException() throw()
    {
    }

    const char* what() const throw()
    {
        return mMessage.c_str();
    }

protected:
    string mMessage;
};


static uint32_t dump_mp4_object_descriptor(uint32_t length);


static bool equals_type(const char *left, const char *right)
{
    return memcmp(left, right, 4) == 0;
}

static void update_atom_read(uint64_t num_read)
{
    MOV_CHECK(!g_atoms.empty());
    MOV_CHECK(num_read <= CURRENT_ATOM.rem_size);
    CURRENT_ATOM.rem_size -= num_read;

    g_file_offset += num_read;
}

static void skip_bytes(uint64_t num_bytes)
{
#if defined(_WIN32)
    MOV_CHECK(_fseeki64(g_mov_file, num_bytes, SEEK_CUR) == 0);
#else
    MOV_CHECK(fseeko(g_mov_file, num_bytes, SEEK_CUR) == 0);
#endif
    update_atom_read(num_bytes);
}

static void push_atom()
{
    MOVAtom atom;
    memset(&atom, 0, sizeof(atom));
    g_atoms.push_back(atom);
}

static void pop_atom()
{
    MOV_CHECK(!g_atoms.empty());
    MOV_CHECK(CURRENT_ATOM.rem_size == 0);

    if (g_atoms.size() > 1)
        PREV_ATOM.rem_size -= CURRENT_ATOM.size;

    g_atoms.pop_back();
}

static bool read_bytes(unsigned char *bytes, uint32_t size)
{
    if (fread(bytes, 1, size, g_mov_file) != size) {
        if (ferror(g_mov_file))
            throw MOVException("Failed to read bytes: %s", strerror(errno));
        return false;
    }

    update_atom_read(size);
    return true;
}

static bool read_uint64(uint64_t *value)
{
    unsigned char bytes[8];
    if (!read_bytes(bytes, 8))
        return false;

    *value = (((uint64_t)bytes[0]) << 56) |
             (((uint64_t)bytes[1]) << 48) |
             (((uint64_t)bytes[2]) << 40) |
             (((uint64_t)bytes[3]) << 32) |
             (((uint64_t)bytes[4]) << 24) |
             (((uint64_t)bytes[5]) << 16) |
             (((uint64_t)bytes[6]) << 8) |
               (uint64_t)bytes[7];

    return true;
}

static bool read_int64(int64_t *value)
{
    uint64_t uvalue;
    if (!read_uint64(&uvalue))
        return false;

    *value = (int64_t)uvalue;
    return true;
}

static bool read_uint32(uint32_t *value)
{
    unsigned char bytes[4];
    if (!read_bytes(bytes, 4))
        return false;

    *value = (((uint32_t)bytes[0]) << 24) |
             (((uint32_t)bytes[1]) << 16) |
             (((uint32_t)bytes[2]) << 8) |
               (uint32_t)bytes[3];

    return true;
}

static bool read_int32(int32_t *value)
{
    uint32_t uvalue;
    if (!read_uint32(&uvalue))
        return false;

    *value = (int32_t)uvalue;
    return true;
}

static bool read_uint24(uint32_t *value)
{
    unsigned char bytes[3];
    if (!read_bytes(bytes, 3))
        return false;

    *value = (((uint32_t)bytes[0]) << 16) |
             (((uint32_t)bytes[1]) << 8) |
               (uint32_t)bytes[2];

    return true;
}

static bool read_int24(int32_t *value)
{
    uint32_t uvalue;
    if (!read_uint24(&uvalue))
        return false;

    *value = (int32_t)uvalue;
    return true;
}

static bool read_uint16(uint16_t *value)
{
    unsigned char bytes[2];
    if (!read_bytes(bytes, 2))
        return false;

    *value = (((uint16_t)bytes[0]) << 8) |
               (uint16_t)bytes[1];

    return true;
}

static bool read_int16(int16_t *value)
{
    uint16_t uvalue;
    if (!read_uint16(&uvalue))
        return false;

    *value = (int16_t)uvalue;
    return true;
}

static bool read_uint8(uint8_t *value)
{
    unsigned char bytes[1];
    if (!read_bytes(bytes, 1))
        return false;

    *value = bytes[0];

    return true;
}

static bool read_int8(int8_t *value)
{
    uint8_t uvalue;
    if (!read_uint8(&uvalue))
        return false;

    *value = (int8_t)uvalue;
    return true;
}

static void write_avcc_ps(unsigned char **buffer, size_t *buffer_size, uint8_t length_size, uint16_t ps_size)
{
    if (!g_avcc_file) {
        g_avcc_file = fopen(g_avcc_filename, "wb");
        if (!g_avcc_file)
            throw MOVException("Failed to open avcc file '%s': %s", g_avcc_filename, strerror(errno));
    }

    unsigned char length_bytes[4];
    if (length_size == 1) {
        length_bytes[0] = (unsigned char)(ps_size & 0xff);
    } else if (length_size == 2) {
        length_bytes[0] = (unsigned char)((ps_size >> 8) & 0xff);
        length_bytes[1] = (unsigned char)( ps_size       & 0xff);
    } else if (length_size == 3) {
        length_bytes[0] = 0; // ps_size is uint16_t
        length_bytes[1] = (unsigned char)((ps_size >>  8) & 0xff);
        length_bytes[2] = (unsigned char)( ps_size        & 0xff);
    } else { // length_size == 4
        length_bytes[0] = 0; // ps_size is uint16_t
        length_bytes[1] = 0; // ps_size is uint16_t
        length_bytes[2] = (unsigned char)((ps_size >>  8) & 0xff);
        length_bytes[3] = (unsigned char)( ps_size        & 0xff);
    }
    MOV_CHECK(fwrite(length_bytes, 1, length_size, g_avcc_file) == length_size);

    if (ps_size > 0) {
        if ((*buffer_size) < ps_size) {
            size_t new_buffer_size = (ps_size + 255) & ~255;
            void *new_buffer = realloc((*buffer), new_buffer_size);
            if (!new_buffer)
                throw MOVException("Failed to allocate buffer");
            *buffer = (unsigned char*)new_buffer;
            *buffer_size = new_buffer_size;
        }
        MOV_CHECK(read_bytes(*buffer, ps_size));
        MOV_CHECK(fwrite(*buffer, 1, ps_size, g_avcc_file) == ps_size);
    }
}

static bool read_type(char *type)
{
    uint32_t value;
    if (!read_uint32(&value))
        return false;

    type[0] = (char)((value >> 24) & 0xff);
    type[1] = (char)((value >> 16) & 0xff);
    type[2] = (char)((value >> 8) & 0xff);
    type[3] = (char)(value & 0xff);

    return true;
}

static bool read_atom_start()
{
    MOVAtom &atom = CURRENT_ATOM;
    atom.size = 8;
    atom.rem_size = 8;
    atom.offset = g_file_offset;

    uint32_t uint32_size;
    uint64_t uint64_size;

    if (!read_uint32(&uint32_size))
        return false; // end-of-file

    MOV_CHECK(read_type(atom.type));
    MOV_CHECK(uint32_size == 1 || uint32_size >= 8);
    if (uint32_size == 1) {
        // extended size
        atom.size += 8;
        atom.rem_size += 8;
        MOV_CHECK(read_uint64(&uint64_size));
    } else {
        uint64_size = uint32_size;
    }

    atom.rem_size = uint64_size - atom.size;
    atom.size = uint64_size;

    return true;
}

static bool read_matrix(uint32_t *value)
{
    int i;
    for (i = 0; i < 9; i++)
        MOV_CHECK(read_uint32(&value[i]));

    return true;
}

static const char* get_profile_string(uint8_t profile_idc, uint8_t constraint_flags_byte)
{
    typedef struct
    {
        uint8_t profile_idc;
        uint8_t flags_mask;
        const char *profile_str;
    } ProfileName;

    static const ProfileName PROFILE_NAMES[] =
    {
        { 66,   0x40,   "Constrained Baseline"},
        { 66,   0x00,   "Baseline"},
        { 77,   0x00,   "Main"},
        { 88,   0x00,   "Extended"},
        {100,   0x00,   "High"},
        {110,   0x10,   "High 10 Intra"},
        {110,   0x00,   "High 10"},
        {122,   0x10,   "High 4:2:2 Intra"},
        {122,   0x00,   "High 4:2:2"},
        {244,   0x10,   "High 4:4:4 Intra"},
        {244,   0x00,   "High 4:4:4"},
        { 44,   0x00,   "CAVLC 4:4:4 Intra"},
    };

    const char *profile_str = "unknown";
    size_t i;
    for (i = 0; i < ARRAY_SIZE(PROFILE_NAMES); i++) {
        if (profile_idc == PROFILE_NAMES[i].profile_idc &&
            (PROFILE_NAMES[i].flags_mask == 0 ||
                (constraint_flags_byte & PROFILE_NAMES[i].flags_mask)))
        {
            profile_str = PROFILE_NAMES[i].profile_str;
            break;
        }
    }

    return profile_str;
}

static const char* get_chroma_format_string(uint8_t chroma_format_idc)
{
    static const char *CHROMA_FORMAT_STRINGS[] =
    {
        "Monochrome",
        "4:2:0",
        "4:2:2",
        "4:4:4",
    };

    return CHROMA_FORMAT_STRINGS[chroma_format_idc & 0x03];
}

static double get_duration_sec(int64_t duration, uint32_t timescale)
{
  if (timescale)
    return duration / (double)timescale;
  else
    return 0.0;
}

static void indent_atom_header()
{
    size_t i;
    for (i = 1; i < g_atoms.size(); i++)
        printf(ATOM_INDENT);
}

static void indent(int extra_amount = 0)
{
    int i;
    for (i = 1; i < (int)g_atoms.size(); i++)
        printf(ATOM_INDENT);
    printf(ATOM_VALUE_INDENT);
    for (i = 0; i < extra_amount; i++)
        printf(" ");
}

static void dump_uint64_index(uint64_t count, uint64_t index)
{
    if (count < 0xffff)
        printf("%04" PRIx64, index);
    else if (count < 0xffffff)
        printf("%06" PRIx64, index);
    else if (count < 0xffffffff)
        printf("%08" PRIx64, index);
    else
        printf("%016" PRIx64, index);
}

static void dump_uint32_index(uint32_t count, uint32_t index)
{
    if (count < 0xffff)
        printf("%04x", index);
    else if (count < 0xffffff)
        printf("%06x", index);
    else
        printf("%08x", index);
}

static void dump_uint16_index(uint16_t count, uint16_t index)
{
    if (count < 0xff)
        printf("%02x", index);
    else
        printf("%04x", index);
}

static void dump_inline_bytes(unsigned char *bytes, uint32_t size)
{
    printf("(size %u) ", size);

    uint32_t i;
    for (i = 0; i < size; i++)
        printf(" %02x", bytes[i]);

    printf("  |");
    for (i = 0; i < size; i++) {
        if (isprint(bytes[i]))
            printf("%c", bytes[i]);
        else
            printf(".");
    }
    printf("|");
}

static void dump_bytes_line(uint64_t size, uint64_t offset, unsigned char *line, uint32_t line_size)
{
    dump_uint64_index(size, offset);
    printf("  ");

    uint32_t i;
    for (i = 0; i < line_size; i++) {
        if (i == 8)
            printf(" ");
        printf(" %02x", line[i]);
    }
    for (; i < 16; i++) {
        if (i == 8)
            printf(" ");
        printf("   ");
    }

    printf("  |");
    for (i = 0; i < line_size; i++) {
        if (isprint(line[i]))
            printf("%c", line[i]);
        else
            printf(".");
    }
    printf("|");
}

static void dump_bytes(uint64_t size, int extra_indent_amount = 0)
{
    if (size == 0)
        return;


    indent(extra_indent_amount);

    unsigned char buffer[16];
    uint64_t total_read = 0;
    uint32_t num_read;
    while (total_read < size) {
        num_read = 16;
        if (total_read + num_read > size)
            num_read = (uint32_t)(size - total_read);
        MOV_CHECK(read_bytes(buffer, num_read));

        if (total_read > 0) {
            printf("\n");
            indent(extra_indent_amount);
        }

        dump_bytes_line(size, total_read, buffer, num_read);

        total_read += num_read;
    }

    printf("\n");
}

static void dump_bytes(unsigned char *bytes, uint64_t size, int extra_indent_amount = 0)
{
    indent(extra_indent_amount);

    uint64_t num_lines = size / 16;
    uint64_t i;
    for (i = 0; i < num_lines; i++) {
        if (i > 0) {
            printf("\n");
            indent(extra_indent_amount);
        }

        dump_bytes_line(size, i * 16, &bytes[i * 16], 16);
    }

    if (num_lines > 0)
        printf("\n");

    if ((size % 16) > 0) {
        if (num_lines > 0)
            indent(extra_indent_amount);
        dump_bytes_line(size, num_lines * 16, &bytes[num_lines * 16], (uint32_t)(size % 16));
        printf("\n");
    }
}

static void dump_string(uint64_t size, int extra_indent_amount = 0)
{
    if (size == 0) {
        printf("\n");
        return;
    }

    if (size > 256) {
        printf("\n");
        indent(extra_indent_amount);
        dump_bytes(size, extra_indent_amount);
        return;
    }

    unsigned char buffer[256];
    MOV_CHECK(read_bytes(buffer, (uint32_t)size));

    uint64_t i;
    for (i = 0; i < size; i++) {
        if (!isprint(buffer[i]))
            break;
    }
    if (i < size) {
        for (; i < size; i++) {
            if (buffer[i] != '\0')
                break;
        }
        if (i < size) {
            printf("\n");
            dump_bytes(buffer, size, extra_indent_amount);
            return;
        }
    }

    printf("'");
    for (i = 0; i < size; i++) {
        if (buffer[i] == '\0')
            break;

        printf("%c", buffer[i]);
    }
    printf("'");
    if (i < size) {
        printf(" +");
        for (; i < size; i++)
            printf(" 0x00");
    }
    printf("\n");
}

static void dump_type(const char *type)
{
    size_t i;
    for (i = 0; i < 4; i++)
        printf("%c", type[i]);
}

static void dump_uint32_tag(uint32_t value)
{
    int i;
    for (i = 3; i >= 0; i--)
        printf("%c", (value >> (8 * i)) & 0xff);
}

static void dump_file_size(uint64_t value)
{
    if (value > UINT32_MAX)
        printf("%20" PRIu64 " (0x%016" PRIx64 ")", value, value);
    else
        printf("%10u (0x%08x)", (uint32_t)value, (uint32_t)value);
}

static void dump_uint64_size(uint64_t value)
{
    printf("%20" PRIu64 " (0x%016" PRIx64 ")", value, value);
}

static void dump_uint32_size(uint32_t value)
{
    printf("%10u (0x%08x)", value, value);
}

static void dump_uint64(uint64_t value, bool hex)
{
    if (hex)
        printf("0x%016" PRIx64, value);
    else
        printf("%20" PRIu64, value);
}

static void dump_int64(int64_t value)
{
    printf("%20" PRId64, value);
}

static void dump_uint32(uint32_t value, bool hex)
{
    if (hex)
        printf("0x%08x", value);
    else
        printf("%10u", value);
}

static void dump_int32(uint32_t value)
{
    printf("%10d", value);
}

static void dump_uint16(uint16_t value, bool hex)
{
    if (hex)
        printf("0x%04x", value);
    else
        printf("%5u", value);
}

static void dump_uint8(uint8_t value, bool hex)
{
    if (hex)
        printf("0x%02x", value);
    else
        printf("%3u", value);
}

static void dump_uint32_chars(uint32_t value)
{
    int i;
    for (i = 3; i >= 0; i--) {
        unsigned char c = (value >> (8 * i)) & 0xff;
        if (isprint(c))
            printf("%c", c);
        else
            printf(".");
    }
    printf(" (");
    for (i = 3; i >= 0; i--) {
        unsigned char c = (value >> (8 * i)) & 0xff;
        if (i != 3)
            printf(" ");
        printf("%02x", c);
    }
    printf(")");
}

static void dump_language(uint16_t value)
{
    unsigned char letter_1 = (unsigned char)((value >> 10) & 0x1f);
    unsigned char letter_2 = (unsigned char)((value >> 5)  & 0x1f);
    unsigned char letter_3 = (unsigned char)( value        & 0x1f);

    if (letter_1 >= 1 && letter_1 <= 26 &&
        letter_2 >= 1 && letter_2 <= 26 &&
        letter_3 >= 1 && letter_3 <= 26)
    {
        printf("0x%04x (%c%c%c)", value, letter_1 + 0x60, letter_2 + 0x60, letter_3 + 0x60);
    }
    else
    {
        printf("0x%04x", value);
    }
}

static void dump_uint32_fp(uint32_t value, uint8_t bits_left)
{
    printf("%f", value / (double)(1 << (32 - bits_left)));
}

static void dump_uint16_fp(uint16_t value, uint8_t bits_left)
{
    printf("%f", value / (double)(1 << (16 - bits_left)));
}

static void dump_int16_fp(int16_t value, uint8_t bits_left)
{
    printf("%f", value / (double)(1 << (16 - bits_left)));
}

static void dump_timestamp(uint64_t value)
{
    // 2082844800 = difference between Unix epoch (1970-01-01) and Apple epoch (1904-01-01)
    time_t unix_secs = (time_t)(value - 2082844800);
    struct tm *utc;
    utc = gmtime(&unix_secs);
    if (utc == 0) {
        printf("%" PRIu64 " seconds since 1904-01-01", value);
    } else {
        printf("%04d-%02d-%02dT%02d:%02d:%02dZ (%" PRIu64 " sec since 1904-01-01)",
               utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
               utc->tm_hour, utc->tm_min, utc->tm_sec,
               value);
    }
}

static void dump_matrix(uint32_t *matrix, int extra_indent_amount = 0)
{
    // matrix:
    //    a  b  u
    //    c  d  v
    //    tx ty w
    //
    // order is: a, b, u, c, d, v, tx, ty, w
    // all are fixed point 16.16, except u, v and w which are 2.30, hence w = 0x40000000 (1.0)

    int i, j;
    for (i = 0; i < 3; i++) {
        indent(extra_indent_amount);
        for (j = 0; j < 3; j++) {
            if (j != 0)
                printf(" ");
            if (j == 2)
                dump_uint32_fp(matrix[i * 3 + j], 2);
            else
                dump_uint32_fp(matrix[i * 3 + j], 16);
        }
        printf("\n");
    }
}

static void dump_color(uint16_t red, uint16_t green, uint16_t blue)
{
    printf("RGB(0x%04x,0x%04x,0x%04x)", red, green, blue);
}

static void dump_fragment_sample_flags(uint32_t flags)
{
    printf("res=0x%x, ",       (flags >> 28) &   0x0f);
    printf("lead=0x%x, ",      (flags >> 26) &   0x03);
    printf("deps_on=0x%x, ",   (flags >> 24) &   0x03);
    printf("depd_on=0x%x, ",   (flags >> 22) &   0x03);
    printf("red=0x%x, ",       (flags >> 20) &   0x03);
    printf("pad=0x%x, ",       (flags >> 17) &   0x07);
    printf("nsync=0x%x, ",     (flags >> 16) &   0x01);
    printf("priority=0x%04x",   flags        & 0xffff);
}

static void dump_atom_header()
{
    indent_atom_header();
    dump_type(CURRENT_ATOM.type);
    printf(": s=");
    dump_file_size(CURRENT_ATOM.size);
    printf(", o=");
    dump_file_size(CURRENT_ATOM.offset);
    printf("\n");
}

static void dump_atom()
{
    dump_atom_header();

    if (CURRENT_ATOM.rem_size > 0)
        dump_bytes(CURRENT_ATOM.rem_size);
}

static void dump_child_atom(const DumpFuncMap *dump_func_map, size_t dump_func_map_size)
{
    size_t i;
    for (i = 0; i < dump_func_map_size; i++) {
        if (dump_func_map[i].type[0] == '\0' || // any type
            (dump_func_map[i].type[0] == (char)0xa9 && dump_func_map[i].type[1] == '\0' && // any international text type
                 CURRENT_ATOM.type[0] == (char)0xa9) ||
            memcmp(dump_func_map[i].type, CURRENT_ATOM.type, 4) == 0)
        {
            dump_func_map[i].dump();
            if (CURRENT_ATOM.rem_size > 0) {
                indent();
                printf("remainder...: %" PRIu64 " unparsed bytes\n", CURRENT_ATOM.rem_size);
                dump_bytes(CURRENT_ATOM.rem_size, 2);
            }
            break;
        }
    }
    if (i >= dump_func_map_size)
        dump_atom();
}

static void dump_container_atom(const DumpFuncMap *dump_func_map, size_t dump_func_map_size)
{
    dump_atom_header();

    while (CURRENT_ATOM.rem_size > 0) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_child_atom(dump_func_map, dump_func_map_size);

        pop_atom();
    }
}

static void dump_full_atom_header(uint8_t *version, uint32_t *flags, bool newline_flags = true)
{
    dump_atom_header();

    MOV_CHECK(read_uint8(version));
    indent();
    printf("version: %u\n", (*version));

    MOV_CHECK(read_uint24(flags));
    indent();
    printf("flags: 0x%06x", (*flags));
    if (newline_flags)
        printf("\n");
}

static void dump_unknown_version(uint8_t version)
{
    indent();
    printf("remainder...: unknown version %u, %" PRIu64 " unparsed bytes\n", version, CURRENT_ATOM.rem_size);
    dump_bytes(CURRENT_ATOM.rem_size, 2);
}


static void dump_ftyp_styp_atom()
{
    dump_atom_header();

    uint32_t major_brand;
    MOV_CHECK(read_uint32(&major_brand));
    g_qt_brand = (major_brand == MKTAG("qt  "));
    indent();
    printf("major_brand: ");
    dump_uint32_chars(major_brand);
    printf("\n");

    uint32_t minor_version;
    MOV_CHECK(read_uint32(&minor_version));
    indent();
    printf("minor_version: ");
    dump_uint32(minor_version, true);
    printf("\n");

    bool first = true;
    uint32_t compatible_brand;
    indent();
    printf("compatible_brands: ");
    while (CURRENT_ATOM.rem_size >= 4) {
        MOV_CHECK(read_uint32(&compatible_brand));
        if (!first)
            printf(", ");
        else
            first = false;
        dump_uint32_chars(compatible_brand);
    }
    printf("\n");
}

static void dump_mdat_atom()
{
    dump_atom_header();

    if (CURRENT_ATOM.rem_size > 0) {
        indent();
        printf("...skipped %" PRIu64 " bytes\n", CURRENT_ATOM.rem_size);
        skip_bytes(CURRENT_ATOM.rem_size);
    }
}

static void dump_free_atom()
{
    dump_atom_header();

    if (CURRENT_ATOM.rem_size > 0) {
        indent();
        printf("...skipped %" PRIu64 " bytes\n", CURRENT_ATOM.rem_size);
        skip_bytes(CURRENT_ATOM.rem_size);
    }
}

static void dump_skip_atom()
{
    dump_atom_header();

    if (CURRENT_ATOM.rem_size > 0) {
        indent();
        printf("...skipped %" PRIu64 " bytes\n", CURRENT_ATOM.rem_size);
        skip_bytes(CURRENT_ATOM.rem_size);
    }
}

static void dump_dref_child_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags, false);
    if ((flags & 0x000001))
        printf(" (self reference)\n");
    else
        printf("\n");

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    if (equals_type(CURRENT_ATOM.type, "url ")) {
        indent();
        printf("data: (%" PRIu64 " bytes) url: ", CURRENT_ATOM.rem_size);
        dump_string(CURRENT_ATOM.rem_size);
        return;
    } else {
        indent();
        printf("data: (%" PRIu64 " bytes)\n", CURRENT_ATOM.rem_size);
        dump_bytes(CURRENT_ATOM.rem_size, 2);
        return;
    }
}

static void dump_dref_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    uint32_t i;
    for (i = 0; i < num_entries; i++) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_dref_child_atom();

        pop_atom();
    }
}

static void dump_stts_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("       count   duration\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint32_t sample_count;
            MOV_CHECK(read_uint32(&sample_count));
            uint32_t sample_duration;
            MOV_CHECK(read_uint32(&sample_duration));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            dump_uint32(sample_count, true);
            printf(" ");
            dump_uint32(sample_duration, true);
            printf("\n");
        }
    }
}

static void dump_ctts_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("       count     offset\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint32_t sample_count;
            MOV_CHECK(read_uint32(&sample_count));
            int32_t sample_offset;
            MOV_CHECK(read_int32(&sample_offset));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            dump_uint32(sample_count, true);
            printf(" ");
            dump_int32(sample_offset);
            printf("\n");
        }
    }
}

static void dump_cslg_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    int32_t dts_shift;
    MOV_CHECK(read_int32(&dts_shift));
    indent();
    printf("dts_shift: %d\n", dts_shift);

    int32_t min_cts;
    MOV_CHECK(read_int32(&min_cts));
    indent();
    printf("min_cts: %d\n", min_cts);

    int32_t max_cts;
    MOV_CHECK(read_int32(&max_cts));
    indent();
    printf("max_cts: %d\n", max_cts);

    int32_t pts_start;
    MOV_CHECK(read_int32(&pts_start));
    indent();
    printf("pts_start: %d\n", pts_start);

    int32_t pts_end;
    MOV_CHECK(read_int32(&pts_end));
    indent();
    printf("pts_end: %d\n", pts_end);
}

static void dump_stss_stps_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("      sample\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint32_t sample;
            MOV_CHECK(read_uint32(&sample));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            dump_uint32(sample, true);
            printf("\n");
        }
    }
}

static void dump_sdtp_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries = (uint32_t)CURRENT_ATOM.rem_size;
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("    is_leading  depends  dependent  redundancy\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint8_t sample;
            MOV_CHECK(read_uint8(&sample));

            // NOTE: not sure whether this is correct for qt
            // sample files had reserved == 1 for I and P-frames, and depends_on only 2 when I-frame
            uint8_t is_leading = (sample & 0xc0) >> 6;
            uint8_t depends_on = (sample & 0x30) >> 4;
            uint8_t dependent_on = (sample & 0x0c) >> 2;
            uint8_t has_redundancy = (sample & 0x03);

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            printf("           %d        %d          %d           %d\n",
                   is_leading, depends_on, dependent_on, has_redundancy);
        }
    }
}

static void dump_stsc_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("  first chunk  samples-per-chunk         descr. id\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint32_t first_chunk;
            MOV_CHECK(read_uint32(&first_chunk));
            uint32_t samples_per_chunk;
            MOV_CHECK(read_uint32(&samples_per_chunk));
            uint32_t sample_description_id;
            MOV_CHECK(read_uint32(&sample_description_id));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            printf(" ");
            dump_uint32(first_chunk, true);
            printf("         ");
            dump_uint32(samples_per_chunk, true);
            printf("        ");
            dump_uint32(sample_description_id, false);
            printf("\n");
        }
    }
}

static void dump_stsz_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t sample_size;
    MOV_CHECK(read_uint32(&sample_size));
    indent();
    printf("sample_size: %d\n", sample_size);

    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (CURRENT_ATOM.rem_size == 0) {
        if (num_entries > 0) {
            indent(4);
            printf("...none\n");
        }
        MOV_CHECK(sample_size > 0 || num_entries == 0);
        return;
    }

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("         size\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            int32_t size;
            MOV_CHECK(read_int32(&size));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            printf(" ");
            dump_uint32(size, true);
            printf("\n");
        }
    }
}

static void dump_stco_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("      offset (hex offset)\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint32_t offset;
            MOV_CHECK(read_uint32(&offset));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            dump_uint32_size(offset);
            printf("\n");
        }
    }
}

static void dump_co64_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("entries (");
    dump_uint32(num_entries, false);
    printf("):\n");

    if (num_entries > 0) {
        indent(4);
        if (num_entries < 0xffff)
            printf("   i");
        else if (num_entries < 0xffffff)
            printf("     i");
        else
            printf("       i");
        printf("                offset         (hex offset)\n");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            uint64_t offset;
            MOV_CHECK(read_uint64(&offset));

            indent(4);
            dump_uint32_index(num_entries, i);
            printf("  ");

            dump_uint64_size(offset);
            printf("\n");
        }
    }
}

static void dump_hdlr_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t component_type;
    MOV_CHECK(read_uint32(&component_type));
    indent();
    printf("component_type: ");
    dump_uint32_chars(component_type);
    printf("\n");

    uint32_t component_sub_type;
    MOV_CHECK(read_uint32(&component_sub_type));
    indent();
    printf("component_sub_type: ");
    dump_uint32_tag(component_sub_type);
    printf("\n");

    if (HAVE_PREV_ATOM && strncmp(PREV_ATOM.type, "mdia", 4) == 0) {
        g_component_type = component_type;
        g_component_sub_type = component_sub_type;
    }

    uint32_t component_manufacturer;
    MOV_CHECK(read_uint32(&component_manufacturer));
    indent();
    printf("component_manufacturer: %u\n", component_manufacturer);

    uint32_t component_flags;
    MOV_CHECK(read_uint32(&component_flags));
    indent();
    printf("component_flags: 0x%08x\n", component_flags);

    uint32_t component_flags_mask;
    MOV_CHECK(read_uint32(&component_flags_mask));
    indent();
    printf("component_flags_mask: 0x%08x\n", component_flags_mask);

    if (CURRENT_ATOM.rem_size > 0) {
        uint64_t component_name_len;
        if (g_qt_brand) {
            uint8_t qt_component_name_len;
            MOV_CHECK(read_uint8(&qt_component_name_len));
            component_name_len = qt_component_name_len;
        } else {
            component_name_len = CURRENT_ATOM.rem_size;
        }
        indent();
        printf("component_name: ");
        if (component_name_len == 0) {
            printf("\n");
        } else {
            dump_string(component_name_len, 2);
        }
    }
}

static void dump_international_string_atom()
{
    dump_atom_header();

    uint16_t len;
    MOV_CHECK(read_uint16(&len));
    uint16_t language_code;
    MOV_CHECK(read_uint16(&language_code));

    indent();
    printf("value: (len=%d,lang=0x%04x) ", len, language_code);
    dump_string(len, 2);
}

static void dump_colr_atom()
{
    dump_atom_header();

    uint32_t color_param_type;
    MOV_CHECK(read_uint32(&color_param_type));
    indent();
    printf("color_param_type: ");
    dump_uint32_tag(color_param_type);
    printf("\n");

    if (color_param_type == MKTAG("nclc")) {
        uint16_t primaries;
        MOV_CHECK(read_uint16(&primaries));
        indent();
        printf("primaries: %u\n", primaries);

        uint16_t transfer_func;
        MOV_CHECK(read_uint16(&transfer_func));
        indent();
        printf("transfer_func: %u\n", transfer_func);

        uint16_t matrix;
        MOV_CHECK(read_uint16(&matrix));
        indent();
        printf("matrix: %u\n", matrix);
    }
}

static void dump_fiel_atom()
{
    dump_atom_header();

    uint8_t fields;
    MOV_CHECK(read_uint8(&fields));
    indent();
    printf("fields: %u", fields);
    if (fields == 1)
        printf(" (progressive)\n");
    else if (fields == 2)
        printf(" (interlaced)\n");
    else
        printf("\n");

    uint8_t detail;
    MOV_CHECK(read_uint8(&detail));
    indent();
    printf("detail: %u\n", detail);
}

static void dump_pasp_atom()
{
    dump_atom_header();

    int32_t h_spacing;
    MOV_CHECK(read_int32(&h_spacing));
    indent();
    printf("h_spacing: %d\n", h_spacing);

    int32_t v_spacing;
    MOV_CHECK(read_int32(&v_spacing));
    indent();
    printf("v_spacing: %d\n", v_spacing);
}

static void dump_clap_atom()
{
    dump_atom_header();

    int32_t clean_ap_width_num, clean_ap_width_den;
    MOV_CHECK(read_int32(&clean_ap_width_num));
    MOV_CHECK(read_int32(&clean_ap_width_den));
    indent();
    printf("clean_aperture_width: %d/%d\n", clean_ap_width_num, clean_ap_width_den);

    int32_t clean_ap_height_num, clean_ap_height_den;
    MOV_CHECK(read_int32(&clean_ap_height_num));
    MOV_CHECK(read_int32(&clean_ap_height_den));
    indent();
    printf("clean_aperture_height: %d/%d\n", clean_ap_height_num, clean_ap_height_den);

    int32_t horiz_offset_num, horiz_offset_den;
    MOV_CHECK(read_int32(&horiz_offset_num));
    MOV_CHECK(read_int32(&horiz_offset_den));
    indent();
    printf("horiz_offset: %d/%d\n", horiz_offset_num, horiz_offset_den);

    int32_t vert_offset_num, vert_offset_den;
    MOV_CHECK(read_int32(&vert_offset_num));
    MOV_CHECK(read_int32(&vert_offset_den));
    indent();
    printf("vert_offset: %d/%d\n", vert_offset_num, vert_offset_den);
}

static void dump_avcc_atom()
{
    dump_atom_header();

    uint8_t configuration_version;
    MOV_CHECK(read_uint8(&configuration_version));
    indent();
    printf("configuration_version: %u\n", configuration_version);

    uint8_t profile_idc;
    MOV_CHECK(read_uint8(&profile_idc));
    uint8_t constraint_flags_byte;
    MOV_CHECK(read_uint8(&constraint_flags_byte));
    indent();
    printf("profile_idc: %u ('%s')\n", profile_idc, get_profile_string(profile_idc, constraint_flags_byte));
    indent();
    printf("constraint_flags_byte: ");
    dump_uint8(constraint_flags_byte, true);
    printf("\n");

    uint8_t level_idc;
    MOV_CHECK(read_uint8(&level_idc));
    indent();
    if (level_idc == 11 && (constraint_flags_byte & 0x10))
        printf("level_idc: %u (1b)\n", level_idc);
    else
        printf("level_idc: %u (%.1f)\n", level_idc, level_idc / 10.0);

    uint8_t length_size_minus1_byte, length_size;
    MOV_CHECK(read_uint8(&length_size_minus1_byte));
    length_size = (length_size_minus1_byte & 0x03) + 1;
    indent();
    printf("length_size_minus1_byte: 0x%02x (length_size=%u)\n", length_size_minus1_byte, length_size);

    uint8_t num_sps_byte, num_sps;
    MOV_CHECK(read_uint8(&num_sps_byte));
    num_sps = num_sps_byte & 0x1f;
    indent();
    printf("num_sps_byte: 0x%02x (num_sps=%u)\n", num_sps_byte, num_sps);

    unsigned char *buffer = 0;
    size_t buffer_size = 0;
    uint8_t i;
    for (i = 0; i < num_sps; i++) {
        uint16_t sps_size;
        MOV_CHECK(read_uint16(&sps_size));

        indent(4);
        printf("sps %u:\n", i);

        if (g_avcc_filename) {
            write_avcc_ps(&buffer, &buffer_size, length_size, sps_size);
            dump_bytes(buffer, sps_size, 6);
        } else {
            dump_bytes(sps_size, 6);
        }
    }

    uint8_t num_pps;
    MOV_CHECK(read_uint8(&num_pps));
    indent();
    printf("num_pps: %u\n", num_pps);

    for (i = 0; i < num_pps; i++) {
        uint16_t pps_size;
        MOV_CHECK(read_uint16(&pps_size));

        indent(4);
        printf("pps %u:\n", i);
        if (g_avcc_filename) {
            write_avcc_ps(&buffer, &buffer_size, length_size, pps_size);
            dump_bytes(buffer, pps_size, 6);
        } else {
            dump_bytes(pps_size, 6);
        }
    }

    if (CURRENT_ATOM.rem_size >= 4) {
        uint8_t chroma_format_byte, chroma_format;
        MOV_CHECK(read_uint8(&chroma_format_byte));
        chroma_format = chroma_format_byte & 0x03;
        indent();
        printf("chroma_format_byte: 0x%02x (chroma_format=%u '%s')\n", chroma_format_byte, chroma_format,
               get_chroma_format_string(chroma_format));

        uint8_t bit_depth_luma_minus8_byte, bit_depth_luma;
        MOV_CHECK(read_uint8(&bit_depth_luma_minus8_byte));
        bit_depth_luma = (bit_depth_luma_minus8_byte & 0x07) + 8;
        indent();
        printf("bit_depth_luma_minus8_byte: 0x%02x (bit_depth_luma=%u)\n",
               bit_depth_luma_minus8_byte, bit_depth_luma);

        uint8_t bit_depth_chroma_minus8_byte, bit_depth_chroma;
        MOV_CHECK(read_uint8(&bit_depth_chroma_minus8_byte));
        bit_depth_chroma = (bit_depth_chroma_minus8_byte & 0x07) + 8;
        indent();
        printf("bit_depth_chroma_minus8_byte: 0x%02x (bit_depth_chroma=%u)\n",
               bit_depth_chroma_minus8_byte, bit_depth_chroma);

        uint8_t num_sps_ext;
        MOV_CHECK(read_uint8(&num_sps_ext));
        indent();
        printf("num_sps_ext: %u\n", num_sps_ext);

        for (i = 0; i < num_sps_ext; i++) {
            uint16_t sps_ext_size;
            MOV_CHECK(read_uint16(&sps_ext_size));

            indent(4);
            printf("sps ext %u:\n", i);
            if (g_avcc_filename) {
                write_avcc_ps(&buffer, &buffer_size, length_size, sps_ext_size);
                dump_bytes(buffer, sps_ext_size, 6);
            } else {
                dump_bytes(sps_ext_size, 6);
            }
        }
    }

    free(buffer);
}

static void dump_btrt_atom()
{
    dump_atom_header();

    uint32_t buffer_size_db;
    MOV_CHECK(read_uint32(&buffer_size_db));
    indent();
    printf("buffer_size_db: 0x%04x\n", buffer_size_db);

    uint32_t max_bitrate;
    MOV_CHECK(read_uint32(&max_bitrate));
    indent();
    printf("max_bitrate: %u\n", max_bitrate);

    uint32_t avg_bitrate;
    MOV_CHECK(read_uint32(&avg_bitrate));
    indent();
    printf("avg_bitrate: %u\n", avg_bitrate);
}

static uint32_t dump_stbl_vide(uint32_t size)
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'c','o','l','r'}, dump_colr_atom},
        {{'f','i','e','l'}, dump_fiel_atom},
        {{'p','a','s','p'}, dump_pasp_atom},
        {{'c','l','a','p'}, dump_clap_atom},
        {{'a','v','c','C'}, dump_avcc_atom},
        {{'b','t','r','t'}, dump_btrt_atom},
    };

    MOV_CHECK(size <= CURRENT_ATOM.rem_size);
    uint64_t end_atom_rem_size = CURRENT_ATOM.rem_size - size;

    uint16_t version;
    MOV_CHECK(read_uint16(&version));
    indent(2);
    printf("version: %u\n", version);

    uint16_t revision;
    MOV_CHECK(read_uint16(&revision));
    indent(2);
    printf("revision: 0x%04x\n", revision);

    uint32_t vendor;
    MOV_CHECK(read_uint32(&vendor));
    indent(2);
    printf("vendor: ");
    dump_uint32_chars(vendor);
    printf("\n");

    uint32_t temporal_quality;
    MOV_CHECK(read_uint32(&temporal_quality));
    indent(2);
    printf("temporal_quality: 0x%08x\n", temporal_quality);

    uint32_t spatial_quality;
    MOV_CHECK(read_uint32(&spatial_quality));
    indent(2);
    printf("spatial_quality: 0x%08x\n", spatial_quality);

    uint16_t width;
    MOV_CHECK(read_uint16(&width));
    indent(2);
    printf("width: %u\n", width);

    uint16_t height;
    MOV_CHECK(read_uint16(&height));
    indent(2);
    printf("height: %u\n", height);

    uint32_t horizontal_resolution;
    MOV_CHECK(read_uint32(&horizontal_resolution));
    indent(2);
    printf("horizontal_resolution: ");
    dump_uint32_fp(horizontal_resolution, 16);
    printf("\n");

    uint32_t vertical_resolution;
    MOV_CHECK(read_uint32(&vertical_resolution));
    indent(2);
    printf("vertical_resolution: ");
    dump_uint32_fp(vertical_resolution, 16);
    printf("\n");

    uint32_t data_size;
    MOV_CHECK(read_uint32(&data_size));
    indent(2);
    printf("data_size: %u\n", data_size);

    uint16_t frame_count;
    MOV_CHECK(read_uint16(&frame_count));
    indent(2);
    printf("frame_count: %u\n", frame_count);

    uint8_t compressor_name_len;
    MOV_CHECK(read_uint8(&compressor_name_len));
    MOV_CHECK(compressor_name_len - 1 <= 32);
    indent(2);
    printf("compressor_name: ");
    dump_string(31, 4);

    uint16_t depth;
    MOV_CHECK(read_uint16(&depth));
    indent(2);
    printf("depth: %u\n", depth);

    uint16_t color_table_id;
    MOV_CHECK(read_uint16(&color_table_id));
    indent(2);
    printf("color_table_id: 0x%04x\n", color_table_id);

    // extensions
    while (CURRENT_ATOM.rem_size > end_atom_rem_size + 8) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);

        pop_atom();
    }

    MOV_CHECK(CURRENT_ATOM.rem_size >= end_atom_rem_size);
    return (uint32_t)(CURRENT_ATOM.rem_size - end_atom_rem_size);
}

static uint32_t dump_mp4_es_descriptor(uint32_t length)
{
    MOV_CHECK(length >= 3);

    indent(4 * mp4_object_desc_level + 2);
    printf("es_descriptor:\n");

    uint16_t es_id;
    MOV_CHECK(read_uint16(&es_id));
    indent(4 * mp4_object_desc_level + 4);
    printf("es_id: 0x%04x\n", es_id);

    uint8_t flag_bits;
    MOV_CHECK(read_uint8(&flag_bits));
    indent(4 * mp4_object_desc_level + 4);
    printf("stream_dep_flag: %u\n", !!(flag_bits & 0x80));
    indent(4 * mp4_object_desc_level + 4);
    printf("url_flag: %u\n", !!(flag_bits & 0x40));
    indent(4 * mp4_object_desc_level + 4);
    printf("reserved: %u\n", !!(flag_bits & 0x20));
    indent(4 * mp4_object_desc_level + 4);
    printf("stream_priority: 0x%02x\n", flag_bits & 0x1f);

    uint32_t rem_length = length - 3;

    if ((flag_bits & 0x80)) {
        MOV_CHECK(rem_length >= 2);
        uint16_t dependson_es_id;
        MOV_CHECK(read_uint16(&dependson_es_id));
        indent(4 * mp4_object_desc_level + 4);
        printf("dependson_es_id: 0x%04x\n", dependson_es_id);
        rem_length -= 2;
    }

    if ((flag_bits & 0x40)) {
        MOV_CHECK(rem_length >= 1);
        uint8_t url_len;
        MOV_CHECK(read_uint8(&url_len));
        rem_length--;
        MOV_CHECK(rem_length >= url_len);
        indent(4 * mp4_object_desc_level + 4);
        printf("url: ");
        dump_string(url_len, 2);
        rem_length -= url_len;
    }

    while (rem_length > 0) {
        mp4_object_desc_level++;
        rem_length -= dump_mp4_object_descriptor(rem_length);
        mp4_object_desc_level--;
    }

    return length;
}

static uint32_t dump_mp4_dc_descriptor(uint32_t length)
{
    MOV_CHECK(length >= 13);

    indent(4 * mp4_object_desc_level + 2);
    printf("decoder_config:\n");

    uint8_t obj_profile_indication;
    MOV_CHECK(read_uint8(&obj_profile_indication));
    indent(4 * mp4_object_desc_level + 4);
    printf("obj_profile_indication: 0x%02x\n", obj_profile_indication);

    uint8_t stream_bits;
    MOV_CHECK(read_uint8(&stream_bits));
    indent(4 * mp4_object_desc_level + 4);
    printf("stream_type: 0x%02x\n", stream_bits >> 2);
    indent(4 * mp4_object_desc_level + 4);
    printf("up_stream: %u\n", !!(stream_bits & 0x02));
    indent(4 * mp4_object_desc_level + 4);
    printf("reserved: %u\n", !!(stream_bits & 0x01));

    uint32_t buffer_size_db;
    MOV_CHECK(read_uint24(&buffer_size_db));
    indent(4 * mp4_object_desc_level + 4);
    printf("buffer_size_db: %u\n", buffer_size_db);

    uint32_t max_bitrate;
    MOV_CHECK(read_uint32(&max_bitrate));
    indent(4 * mp4_object_desc_level + 4);
    printf("max_bitrate: %u\n", max_bitrate);

    uint32_t avg_bitrate;
    MOV_CHECK(read_uint32(&avg_bitrate));
    indent(4 * mp4_object_desc_level + 4);
    printf("avg_bitrate: %u\n", avg_bitrate);

    uint32_t rem_length = length - 13;
    while (rem_length > 0) {
        mp4_object_desc_level++;
        rem_length -= dump_mp4_object_descriptor(rem_length);
        mp4_object_desc_level--;
    }

    return length;
}

static uint32_t dump_mp4_ds_info(uint32_t length)
{
    indent(4 * mp4_object_desc_level + 2);
    printf("decoder_specific_info:\n");

    dump_bytes(length, 4 * mp4_object_desc_level + 4);

    return length;
}

static uint32_t dump_mp4_slc_descriptor(uint32_t length)
{
    MOV_CHECK(length >= 1);

    indent(4 * mp4_object_desc_level + 2);
    printf("sl_config:\n");

    uint8_t predefined;
    MOV_CHECK(read_uint8(&predefined));
    indent(4 * mp4_object_desc_level + 4);
    printf("predefined: 0x%02x\n", predefined);

    if (length > 1)
        dump_bytes(length - 1, 4 * mp4_object_desc_level + 6);

    return length;
}

static uint32_t dump_mp4_object_descriptor(uint32_t parent_length)
{
    MOV_CHECK(parent_length >= 2);

    indent(4 * mp4_object_desc_level);
    printf("descriptor:\n");

    uint32_t head_length = 0;

    uint8_t tag;
    MOV_CHECK(read_uint8(&tag));
    indent(4 * mp4_object_desc_level + 2);
    printf("tag: 0x%02x\n", tag);
    head_length++;

    uint32_t length = 0;
    uint8_t byte;
    do {
        MOV_CHECK(read_uint8(&byte));
        head_length++;
        length <<= 7;
        length |= byte & 0x7f;
    } while (byte & 0x80);
    indent(4 * mp4_object_desc_level + 2);
    printf("length: %u\n", length);

    MOV_CHECK(parent_length >= head_length + length);

    uint32_t used_length;
    switch (tag)
    {
        case 0x03:
            used_length = dump_mp4_es_descriptor(length);
            break;
        case 0x04:
            used_length = dump_mp4_dc_descriptor(length);
            break;
        case 0x05:
            used_length = dump_mp4_ds_info(length);
            break;
        case 0x06:
            used_length = dump_mp4_slc_descriptor(length);
            break;
        default:
            dump_bytes(length, 4 * mp4_object_desc_level + 4);
            used_length = length;
            break;
    }

    return head_length + used_length;
}

static void dump_esds_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0) {
        dump_unknown_version(version);
        return;
    }

    while (CURRENT_ATOM.rem_size > 2)
        dump_mp4_object_descriptor((uint32_t)CURRENT_ATOM.rem_size);
}

static uint32_t dump_stbl_soun(uint32_t size)
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'e','s','d','s'}, dump_esds_atom},
        {{'b','t','r','t'}, dump_btrt_atom},
    };

    MOV_CHECK(size <= CURRENT_ATOM.rem_size);
    uint64_t end_atom_rem_size = CURRENT_ATOM.rem_size - size;

    uint16_t version;
    MOV_CHECK(read_uint16(&version));
    indent(2);
    printf("version: %u\n", version);

    uint16_t revision;
    MOV_CHECK(read_uint16(&revision));
    indent(2);
    printf("revision: 0x%04x\n", revision);

    uint32_t vendor;
    MOV_CHECK(read_uint32(&vendor));
    indent(2);
    printf("vendor: ");
    dump_uint32_chars(vendor);
    printf("\n");

    uint16_t channel_count;
    MOV_CHECK(read_uint16(&channel_count));
    indent(2);
    printf("channel_count: %u\n", channel_count);

    uint16_t sample_size;
    MOV_CHECK(read_uint16(&sample_size));
    indent(2);
    printf("sample_size: %u\n", sample_size);

    int16_t compression_id;
    MOV_CHECK(read_int16(&compression_id));
    indent(2);
    printf("compression_id: %d\n", compression_id);

    uint16_t packet_size;
    MOV_CHECK(read_uint16(&packet_size));
    indent(2);
    printf("packet_size: %u\n", packet_size);

    uint32_t sample_rate;
    MOV_CHECK(read_uint32(&sample_rate));
    indent(2);
    printf("sample_rate: ");
    dump_uint32_fp(sample_rate, 16);
    printf("\n");

    if (version == 1) {
        uint32_t samples_per_packet;
        MOV_CHECK(read_uint32(&samples_per_packet));
        indent(2);
        printf("samples_per_packet: %u\n", samples_per_packet);

        uint32_t bytes_per_packet;
        MOV_CHECK(read_uint32(&bytes_per_packet));
        indent(2);
        printf("bytes_per_packet: %u\n", bytes_per_packet);

        uint32_t bytes_per_frame;
        MOV_CHECK(read_uint32(&bytes_per_frame));
        indent(2);
        printf("bytes_per_frame: %u\n", bytes_per_frame);

        uint32_t bytes_per_sample;
        MOV_CHECK(read_uint32(&bytes_per_sample));
        indent(2);
        printf("bytes_per_sample: %u\n", bytes_per_sample);
    }

    // extensions
    if (version == 0 || version == 1) { // size is known for these versions and can be sure what follows will be atoms
        while (CURRENT_ATOM.rem_size > end_atom_rem_size + 8) {
            push_atom();

            if (!read_atom_start())
                break;

            dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);

            pop_atom();
        }
    }

    MOV_CHECK(CURRENT_ATOM.rem_size >= end_atom_rem_size);
    return (uint32_t)(CURRENT_ATOM.rem_size - end_atom_rem_size);
}

static uint32_t dump_stbl_tmcd(uint32_t size)
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'n','a','m','e'}, dump_international_string_atom},
    };

    MOV_CHECK(size <= CURRENT_ATOM.rem_size);
    uint64_t end_atom_rem_size = CURRENT_ATOM.rem_size - size;

    uint32_t reserved1;
    MOV_CHECK(read_uint32(&reserved1));
    indent(2);
    printf("reserved: 0x%08x\n", reserved1);

    uint32_t flags;
    MOV_CHECK(read_uint32(&flags));
    indent(2);
    printf("flags: 0x%08x\n", flags);

    uint32_t timescale;
    MOV_CHECK(read_uint32(&timescale));
    indent(2);
    printf("timescale: %u\n", timescale);

    int32_t frame_duration;
    MOV_CHECK(read_int32(&frame_duration));
    indent(2);
    printf("frame_duration: %d (%f sec)\n", frame_duration, get_duration_sec(frame_duration, timescale));

    uint8_t number_of_frames;
    MOV_CHECK(read_uint8(&number_of_frames));
    indent(2);
    printf("number_of_frames: %u\n", number_of_frames);

    uint8_t reserved2;
    MOV_CHECK(read_uint8(&reserved2));
    indent(2);
    printf("reserved: 0x%02x\n", reserved2);

    // extensions
    while (CURRENT_ATOM.rem_size > end_atom_rem_size + 8) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);

        pop_atom();
    }

    MOV_CHECK(CURRENT_ATOM.rem_size >= end_atom_rem_size);
    return (uint32_t)(CURRENT_ATOM.rem_size - end_atom_rem_size);
}

static void dump_stsd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));
    indent();
    printf("sample_descriptions (");
    dump_uint32(num_entries, true);
    printf("):\n");

    uint32_t i;
    for (i = 0; i < num_entries; i++) {
        uint32_t size;
        MOV_CHECK(read_uint32(&size));
        indent(2);
        printf("size: %08x\n", size);
        MOV_CHECK(size >= 16);

        uint32_t data_format;
        MOV_CHECK(read_uint32(&data_format));
        indent(2);
        printf("data_format: ");
        dump_uint32_chars(data_format);
        printf("\n");

        unsigned char reserved[6];
        MOV_CHECK(read_bytes(reserved, 6));
        indent(2);
        printf("reserved: ");
        dump_inline_bytes(reserved, 6);
        printf("\n");

        uint16_t data_ref_index;
        MOV_CHECK(read_uint16(&data_ref_index));
        indent(2);
        printf("data_ref_index: 0x%04x\n", data_ref_index);

        uint32_t rem_size = size - 16;
        if (g_component_type == MHLR_COMPONENT_TYPE || (!g_component_type && !g_qt_brand)) {
            if (g_component_sub_type == VIDE_COMPONENT_SUB_TYPE)
                rem_size = dump_stbl_vide(rem_size);
            else if (g_component_sub_type == SOUN_COMPONENT_SUB_TYPE)
                rem_size = dump_stbl_soun(rem_size);
            else if (g_component_sub_type == TMCD_COMPONENT_SUB_TYPE)
                rem_size = dump_stbl_tmcd(rem_size);
        }
        if (rem_size > 0) {
            indent(2);
            printf("remainder...: %u unparsed bytes\n", rem_size);
            dump_bytes(rem_size, 4);
        }
    }
}

static void dump_stbl_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'s','t','s','d'}, dump_stsd_atom},
        {{'s','t','t','s'}, dump_stts_atom},
        {{'c','t','t','s'}, dump_ctts_atom},
        {{'c','s','l','g'}, dump_cslg_atom},
        {{'s','t','s','s'}, dump_stss_stps_atom},
        {{'s','t','p','s'}, dump_stss_stps_atom},
        {{'s','d','t','p'}, dump_sdtp_atom},
        {{'s','t','s','c'}, dump_stsc_atom},
        {{'s','t','s','z'}, dump_stsz_atom},
        {{'s','t','c','o'}, dump_stco_atom},
        {{'c','o','6','4'}, dump_co64_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_dinf_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'d','r','e','f'}, dump_dref_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_vmhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags, false);
    if ((flags & 0x0001))
        printf(" (no lean ahead)");
    printf("\n");

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint16_t graphics_mode;
    MOV_CHECK(read_uint16(&graphics_mode));
    indent();
    printf("graphics_mode: %02x\n", graphics_mode);

    uint16_t opcolor_r, opcolor_g, opcolor_b;
    MOV_CHECK(read_uint16(&opcolor_r));
    MOV_CHECK(read_uint16(&opcolor_g));
    MOV_CHECK(read_uint16(&opcolor_b));
    indent();
    printf("opcolor: ");
    dump_color(opcolor_r, opcolor_g, opcolor_b);
    printf("\n");
}

static void dump_smhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    int16_t balance;
    MOV_CHECK(read_int16(&balance));
    indent();
    printf("balance: ");
    dump_int16_fp(balance, 8);
    printf("\n");

    uint16_t reserved;
    MOV_CHECK(read_uint16(&reserved));
    indent();
    printf("reserved: ");
    dump_uint16(reserved, true);
    printf("\n");
}

static void dump_gmin_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags, false);
    if ((flags & 0x0001))
        printf(" (no lean ahead)");
    printf("\n");

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint16_t graphics_mode;
    MOV_CHECK(read_uint16(&graphics_mode));
    indent();
    printf("graphics_mode: %02x\n", graphics_mode);

    uint16_t opcolor_r, opcolor_g, opcolor_b;
    MOV_CHECK(read_uint16(&opcolor_r));
    MOV_CHECK(read_uint16(&opcolor_g));
    MOV_CHECK(read_uint16(&opcolor_b));
    indent();
    printf("opcolor: ");
    dump_color(opcolor_r, opcolor_g, opcolor_b);
    printf("\n");

    int16_t balance;
    MOV_CHECK(read_int16(&balance));
    indent();
    printf("balance: ");
    dump_int16_fp(balance, 8);
    printf("\n");

    uint16_t reserved;
    MOV_CHECK(read_uint16(&reserved));
    indent();
    printf("reserved: ");
    dump_uint16(reserved, true);
    printf("\n");
}

static void dump_tcmi_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags, false);
    if ((flags & 0x0001))
        printf(" (no lean ahead)");
    printf("\n");

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint16_t text_font;
    MOV_CHECK(read_uint16(&text_font));
    indent();
    printf("text_font: %02x\n", text_font);

    uint16_t text_face;
    MOV_CHECK(read_uint16(&text_face));
    indent();
    printf("text_face: %02x\n", text_face);

    uint32_t text_size;
    MOV_CHECK(read_uint32(&text_size));
    indent();
    printf("text_size: ");
    dump_uint32_fp(text_size, 16);
    printf("\n");

    uint16_t text_color_r, text_color_g, text_color_b;
    MOV_CHECK(read_uint16(&text_color_r));
    MOV_CHECK(read_uint16(&text_color_g));
    MOV_CHECK(read_uint16(&text_color_b));
    indent();
    printf("text_color: ");
    dump_color(text_color_r, text_color_g, text_color_b);
    printf("\n");

    uint16_t bg_color_r, bg_color_g, bg_color_b;
    MOV_CHECK(read_uint16(&bg_color_r));
    MOV_CHECK(read_uint16(&bg_color_g));
    MOV_CHECK(read_uint16(&bg_color_b));
    indent();
    printf("background_color: ");
    dump_color(bg_color_r, bg_color_g, bg_color_b);
    printf("\n");

    uint8_t font_name_size;
    MOV_CHECK(read_uint8(&font_name_size));
    indent();
    printf("font_name: ");
    dump_string(font_name_size, 2);
}

static void dump_tmcd_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'t','c','m','i'}, dump_tcmi_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_gmhd_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'g','m','i','n'}, dump_gmin_atom},
        {{'t','m','c','d'}, dump_tmcd_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_minf_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'v','m','h','d'}, dump_vmhd_atom},
        {{'s','m','h','d'}, dump_smhd_atom},
        {{'g','m','h','d'}, dump_gmhd_atom},
        {{'h','d','l','r'}, dump_hdlr_atom},
        {{'d','i','n','f'}, dump_dinf_atom},
        {{'s','t','b','l'}, dump_stbl_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_mdhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00 && version != 0x01) {
        dump_unknown_version(version);
        return;
    }


    if (version == 0x00) {
        uint32_t creation_time;
        MOV_CHECK(read_uint32(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint32_t modification_time;
        MOV_CHECK(read_uint32(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    } else {
        uint64_t creation_time;
        MOV_CHECK(read_uint64(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint64_t modification_time;
        MOV_CHECK(read_uint64(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    }

    uint32_t timescale;
    MOV_CHECK(read_uint32(&timescale));
    indent();
    printf("timescale: %u\n", timescale);

    if (version == 0x00) {
        int32_t duration;
        MOV_CHECK(read_int32(&duration));
        indent();
        printf("duration: %d (%f sec)\n", duration, get_duration_sec(duration, timescale));
    } else {
        int64_t duration;
        MOV_CHECK(read_int64(&duration));
        indent();
        printf("duration: %" PRId64 " (%f sec)\n", duration, get_duration_sec(duration, timescale));
    }

    uint16_t language;
    MOV_CHECK(read_uint16(&language));
    indent();
    printf("language: ");
    dump_language(language);
    printf("\n");

    uint16_t quality;
    MOV_CHECK(read_uint16(&quality));
    indent();
    printf("quality: %u\n", quality);
}

static void dump_mdia_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'m','d','h','d'}, dump_mdhd_atom},
        {{'h','d','l','r'}, dump_hdlr_atom},
        {{'m','i','n','f'}, dump_minf_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_keys_atom()
{
    static uint32_t mdta_key_namespace = MKTAG("mdta");

    g_meta_keys.clear();

    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t count;
    MOV_CHECK(read_uint32(&count));

    indent();
    printf("key_values (");
    dump_uint32(count, false);
    printf("):\n");

    unsigned char key_value_buffer[129];
    uint32_t total_count = 0;
    for (total_count = 0; total_count < count; total_count++) {
        indent(2);

        uint32_t key_size;
        uint32_t key_value_size;
        MOV_CHECK(read_uint32(&key_size));
        MOV_CHECK(key_size >= 8);
        uint32_t key_namespace;
        MOV_CHECK(read_uint32(&key_namespace));

        printf("%4u  ", total_count + 1);
        dump_uint32(key_size, true);

        key_value_size = key_size - 8;

        if (key_namespace == mdta_key_namespace && key_value_size < sizeof(key_value_buffer)) {
            MOV_CHECK(read_bytes(key_value_buffer, key_value_size));

            size_t i;
            for (i = 0; i < key_value_size; i++) {
                if (!isprint(key_value_buffer[i]))
                    break;
            }
            if (i >= key_value_size) {
                key_value_buffer[key_value_size] = '\0';
                printf("  mdta  '%s'\n", (char*)key_value_buffer);
                g_meta_keys.push_back((char*)key_value_buffer);
            } else {
                printf("  ");
                dump_uint32_chars(key_namespace);
                printf("\n");
                dump_bytes(key_value_buffer, key_value_size, 4);
                g_meta_keys.push_back("");
            }
        } else {
            printf("  ");
            dump_uint32_chars(key_namespace);
            printf("\n");
            dump_bytes(key_value_size, 4);
            g_meta_keys.push_back("");
        }
    }
}

static void dump_ilst_data_atom()
{
    dump_atom_header();

    uint8_t type_field_1;
    uint32_t type_field_2;
    MOV_CHECK(read_uint8(&type_field_1));
    MOV_CHECK(read_uint24(&type_field_2));

    indent();
    printf("type 1: %u\n", type_field_1);
    indent();
    printf("type 2: %u\n", type_field_2);

    uint16_t locale;
    uint16_t country;
    MOV_CHECK(read_uint16(&locale));
    MOV_CHECK(read_uint16(&country));

    indent();
    printf("locale: %u\n", locale);
    indent();
    printf("country: %u\n", country);

    indent();
    if (type_field_1 == 0 &&
        (type_field_2 == 1 ||    // utf-8
         type_field_2 == 21 ||   // be signed integer
         type_field_2 == 22))    // be unsigned integer
    {
        if (type_field_2 == 21) {
            if (CURRENT_ATOM.rem_size == 8) {
                int64_t value;
                MOV_CHECK(read_int64(&value));
                printf("value (int64): %" PRId64 "\n", value);
            } else if (CURRENT_ATOM.rem_size == 4) {
                int32_t value;
                MOV_CHECK(read_int32(&value));
                printf("value (int32): %d\n", value);
            } else if (CURRENT_ATOM.rem_size == 3) {
                int32_t value;
                MOV_CHECK(read_int24(&value));
                printf("value (int24): %d\n", value);
            } else if (CURRENT_ATOM.rem_size == 2) {
                int16_t value;
                MOV_CHECK(read_int16(&value));
                printf("value (int16): %d\n", value);
            } else if (CURRENT_ATOM.rem_size == 1) {
                int8_t value;
                MOV_CHECK(read_int8(&value));
                printf("value (int8): %d\n", value);
            } else {
                printf("value:\n");
                dump_bytes(CURRENT_ATOM.rem_size, 4);
            }
        } else if (type_field_2 == 22) {
            if (CURRENT_ATOM.rem_size == 8) {
                uint64_t value;
                MOV_CHECK(read_uint64(&value));
                printf("value (uint64): %" PRIu64 "\n", value);
            } else if (CURRENT_ATOM.rem_size == 4) {
                uint32_t value;
                MOV_CHECK(read_uint32(&value));
                printf("value (uint32): %u\n", value);
            } else if (CURRENT_ATOM.rem_size == 3) {
                uint32_t value;
                MOV_CHECK(read_uint24(&value));
                printf("value (uint24): %u\n", value);
            } else if (CURRENT_ATOM.rem_size == 2) {
                uint16_t value;
                MOV_CHECK(read_uint16(&value));
                printf("value (uint16): %u\n", value);
            } else if (CURRENT_ATOM.rem_size == 1) {
                uint8_t value;
                MOV_CHECK(read_uint8(&value));
                printf("value (uint8): %u\n", value);
            } else {
                printf("value:\n");
                dump_bytes(CURRENT_ATOM.rem_size, 4);
            }
        } else {    // type_field_2 == 1
            uint64_t utf8_value_size = CURRENT_ATOM.rem_size;
            unsigned char utf8_value_buffer[129];
            if (utf8_value_size == 0) {
                printf("value: ''\n");
            } else if (utf8_value_size < sizeof(utf8_value_buffer)) {
                MOV_CHECK(read_bytes(utf8_value_buffer, (uint32_t)utf8_value_size));

                size_t i;
                for (i = 0; i < utf8_value_size; i++) {
                    if (!isprint(utf8_value_buffer[i]))
                        break;
                }
                if (i >= utf8_value_size) {
                    utf8_value_buffer[utf8_value_size] = '\0';
                    printf("value: '%s'\n", (char*)utf8_value_buffer);
                } else {
                    printf("value:\n");
                    dump_bytes(utf8_value_buffer, utf8_value_size, 4);
                }
            } else {
                printf("value:\n");
                dump_bytes(utf8_value_size, 4);
            }
        }
    }
    else
    {
        printf("value:\n");
        dump_bytes(CURRENT_ATOM.rem_size, 4);
    }
}

static void dump_ilst_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'d','a','t','a'}, dump_ilst_data_atom},
    };

    dump_atom_header();

    while (CURRENT_ATOM.rem_size > 0) {
        uint32_t element_size;
        MOV_CHECK(read_uint32(&element_size));
        uint32_t key_index;
        MOV_CHECK(read_uint32(&key_index));
        MOV_CHECK(key_index >= 1 && (g_meta_keys.empty() || key_index <= g_meta_keys.size()));

        indent();
        printf("size: ");
        dump_uint32_size(element_size);
        printf("\n");
        indent();
        printf("key_index: %u", key_index);
        if (!g_meta_keys.empty()) {
            if (!g_meta_keys[key_index - 1].empty())
                printf(" ('%s')", g_meta_keys[key_index - 1].c_str());
        }
        printf("\n");


        push_atom();

        if (!read_atom_start())
            break;

        dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);

        pop_atom();
    }
}

static void dump_clefprofenof_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00) {
        dump_unknown_version(version);
        return;
    }


    uint32_t fp_width;
    MOV_CHECK(read_uint32(&fp_width));
    indent();
    printf("width: ");
    dump_uint32_fp(fp_width, 16);
    printf("\n");

    uint32_t fp_height;
    MOV_CHECK(read_uint32(&fp_height));
    indent();
    printf("height: ");
    dump_uint32_fp(fp_height, 16);
    printf("\n");
}

static void dump_tapt_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'c','l','e','f'}, dump_clefprofenof_atom},
        {{'p','r','o','f'}, dump_clefprofenof_atom},
        {{'e','n','o','f'}, dump_clefprofenof_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_tref_child_atom()
{
    dump_atom_header();

    uint32_t count = (uint32_t)(CURRENT_ATOM.rem_size / 4);

    indent();
    printf("track_ids (");
    dump_uint32(count, false);
    printf("):\n");

    indent(4);
    if (count < 0xffff)
        printf("   i");
    else if (count < 0xffffff)
        printf("     i");
    else
        printf("       i");
    printf("          id\n");


    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t track_id;
        MOV_CHECK(read_uint32(&track_id));

        indent(4);
        dump_uint32_index(count, i);

        printf("  ");
        dump_uint32(track_id, true);
    }
    printf("\n");
}

static void dump_tref_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'\0','\0','\0','\0'}, dump_tref_child_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_elst_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00 && version != 0x01) {
        dump_unknown_version(version);
        return;
    }


    uint32_t count;
    MOV_CHECK(read_uint32(&count));

    indent();
    printf("edit_list_table (");
    dump_uint32(count, false);
    printf("):\n");

    indent(4);
    if (count < 0xffff)
        printf("   i");
    else if (count < 0xffffff)
        printf("     i");
    else
        printf("       i");
    if (version == 0)
        printf("    duration       time          rate\n");
    else
        printf("              duration                 time          rate\n");


    uint32_t i = 0;
    for (i = 0; i < count; i++) {
        indent(4);
        dump_uint32_index(count, i);
        if (version == 0) {
            uint32_t track_duration;
            MOV_CHECK(read_uint32(&track_duration));
            int32_t media_time;
            MOV_CHECK(read_int32(&media_time));

            printf("  ");
            dump_uint32(track_duration, false);

            printf(" ");
            dump_int32(media_time);
        } else {
            uint64_t track_duration;
            MOV_CHECK(read_uint64(&track_duration));
            int64_t media_time;
            MOV_CHECK(read_int64(&media_time));

            printf("  ");
            dump_uint64(track_duration, false);

            printf(" ");
            dump_int64(media_time);
        }

        uint32_t media_rate;
        MOV_CHECK(read_uint32(&media_rate));

        printf("      ");
        dump_uint32_fp(media_rate, 16);
        printf("\n");
    }
}

static void dump_edts_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'e','l','s','t'}, dump_elst_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_meta_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'h','d','l','r'}, dump_hdlr_atom},
        {{'k','e','y','s'}, dump_keys_atom},
        {{'i','l','s','t'}, dump_ilst_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_tkhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00 && version != 0x01) {
        dump_unknown_version(version);
        return;
    }


    if (version == 0x00) {
        uint32_t creation_time;
        MOV_CHECK(read_uint32(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint32_t modification_time;
        MOV_CHECK(read_uint32(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    } else {
        uint64_t creation_time;
        MOV_CHECK(read_uint64(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint64_t modification_time;
        MOV_CHECK(read_uint64(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    }

    uint32_t track_id;
    MOV_CHECK(read_uint32(&track_id));
    indent();
    printf("track_id: %u\n", track_id);

    uint32_t reserved_uint32;
    MOV_CHECK(read_uint32(&reserved_uint32));
    indent();
    printf("reserved: ");
    dump_uint32(reserved_uint32, true);
    printf("\n");

    if (version == 0x00) {
        int32_t duration;
        MOV_CHECK(read_int32(&duration));
        indent();
        printf("duration: %d (%f sec)\n", duration, get_duration_sec(duration, g_movie_timescale));
    } else {
        int64_t duration;
        MOV_CHECK(read_int64(&duration));
        indent();
        printf("duration: %" PRId64 " (%f sec)\n", duration, get_duration_sec(duration, g_movie_timescale));
    }

    unsigned char reserved_bytes[8];
    MOV_CHECK(read_bytes(reserved_bytes, 8));
    indent();
    printf("reserved: ");
    dump_inline_bytes(reserved_bytes, 8);
    printf("\n");

    uint16_t layer;
    MOV_CHECK(read_uint16(&layer));
    indent();
    printf("layer: %u\n", layer);

    uint16_t alternate_group;
    MOV_CHECK(read_uint16(&alternate_group));
    indent();
    printf("alternate_group: %u\n", alternate_group);

    uint16_t volume;
    MOV_CHECK(read_uint16(&volume));
    indent();
    printf("volume: ");
    dump_uint16_fp(volume, 8);
    printf("\n");

    uint16_t reserved_uint16;
    MOV_CHECK(read_uint16(&reserved_uint16));
    indent();
    printf("reserved: ");
    dump_uint16(reserved_uint16, true);
    printf("\n");

    uint32_t matrix[9];
    MOV_CHECK(read_matrix(matrix));
    indent();
    printf("matrix: \n");
    dump_matrix(matrix, 2);

    uint32_t track_width;
    MOV_CHECK(read_uint32(&track_width));
    indent();
    printf("track_width: ");
    dump_uint32_fp(track_width, 16);
    printf("\n");

    uint32_t track_height;
    MOV_CHECK(read_uint32(&track_height));
    indent();
    printf("track_height: ");
    dump_uint32_fp(track_height, 16);
    printf("\n");
}

static void dump_udta_name_atom()
{
    dump_atom_header();

    indent();
    printf("value: (len=%" PRId64 ") ", CURRENT_ATOM.rem_size);
    dump_string(CURRENT_ATOM.rem_size, 2);
}

static void dump_udta_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'n','a','m','e'},     dump_udta_name_atom},
        {{(char)0xa9,'\0','\0','\0'}, dump_international_string_atom},
    };

    dump_atom_header();

    while (CURRENT_ATOM.rem_size > 8) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);

        pop_atom();
    }
}

static void dump_trak_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'t','k','h','d'}, dump_tkhd_atom},
        {{'t','a','p','t'}, dump_tapt_atom},
        {{'e','d','t','s'}, dump_edts_atom},
        {{'t','r','e','f'}, dump_tref_atom},
        {{'m','d','i','a'}, dump_mdia_atom},
        {{'m','e','t','a'}, dump_meta_atom},
        {{'u','d','t','a'}, dump_udta_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_mvhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);

    if (version != 0x00 && version != 0x01) {
        dump_unknown_version(version);
        return;
    }


    if (version == 0x00) {
        uint32_t creation_time;
        MOV_CHECK(read_uint32(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint32_t modification_time;
        MOV_CHECK(read_uint32(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    } else {
        uint64_t creation_time;
        MOV_CHECK(read_uint64(&creation_time));
        indent();
        printf("creation_time: ");
        dump_timestamp(creation_time);
        printf("\n");

        uint64_t modification_time;
        MOV_CHECK(read_uint64(&modification_time));
        indent();
        printf("modification_time: ");
        dump_timestamp(modification_time);
        printf("\n");
    }

    MOV_CHECK(read_uint32(&g_movie_timescale));
    indent();
    printf("timescale: %u\n", g_movie_timescale);

    if (version == 0x00) {
        int32_t duration;
        MOV_CHECK(read_int32(&duration));
        indent();
        printf("duration: %d (%f sec)\n", duration, get_duration_sec(duration, g_movie_timescale));
    } else {
        int64_t duration;
        MOV_CHECK(read_int64(&duration));
        indent();
        printf("duration: %" PRId64 " (%f sec)\n", duration, get_duration_sec(duration, g_movie_timescale));
    }

    uint32_t preferred_rate;
    MOV_CHECK(read_uint32(&preferred_rate));
    indent();
    printf("preferred_rate: ");
    dump_uint32_fp(preferred_rate, 16);
    printf("\n");

    uint16_t preferred_volume;
    MOV_CHECK(read_uint16(&preferred_volume));
    indent();
    printf("preferred_volume: ");
    dump_uint16_fp(preferred_volume, 8);
    printf("\n");

    unsigned char bytes[10];
    MOV_CHECK(read_bytes(bytes, 10));
    indent();
    printf("reserved: ");
    dump_inline_bytes(bytes, 10);
    printf("\n");

    uint32_t matrix[9];
    MOV_CHECK(read_matrix(matrix));
    indent();
    printf("matrix: \n");
    dump_matrix(matrix, 2);


    // NOTE/TODO: spec. does not clearly state the xxx_time values are in timescale units

    uint32_t preview_time;
    MOV_CHECK(read_uint32(&preview_time));
    indent();
    printf("preview_time: %u\n", preview_time);

    uint32_t preview_duration;
    MOV_CHECK(read_uint32(&preview_duration));
    indent();
    printf("preview_duration: %u (%f sec)\n", preview_duration, get_duration_sec(preview_duration, g_movie_timescale));

    uint32_t poster_time;
    MOV_CHECK(read_uint32(&poster_time));
    indent();
    printf("poster_time: %u\n", poster_time);

    uint32_t selection_time;
    MOV_CHECK(read_uint32(&selection_time));
    indent();
    printf("selection_time: %u\n", selection_time);

    uint32_t selection_duration;
    MOV_CHECK(read_uint32(&selection_duration));
    indent();
    printf("selection_duration: %u (%f sec)\n", selection_duration, get_duration_sec(selection_duration, g_movie_timescale));

    uint32_t current_time;
    MOV_CHECK(read_uint32(&current_time));
    indent();
    printf("current_time: %u\n", current_time);

    uint32_t next_track_id;
    MOV_CHECK(read_uint32(&next_track_id));
    indent();
    printf("next_track_id: %u\n", next_track_id);
}

static void dump_mehd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    if (version == 0) {
        uint32_t fragment_duration;
        MOV_CHECK(read_uint32(&fragment_duration));
        indent();
        printf("fragment_duration: ");
        dump_uint32(fragment_duration, true);
        printf("\n");
    } else {
        uint64_t fragment_duration;
        MOV_CHECK(read_uint64(&fragment_duration));
        indent();
        printf("fragment_duration: ");
        dump_uint64(fragment_duration, true);
        printf("\n");
    }
}

static void dump_trex_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t track_id;
    MOV_CHECK(read_uint32(&track_id));
    indent();
    printf("track_id: %u\n", track_id);

    uint32_t default_sample_description_index;
    MOV_CHECK(read_uint32(&default_sample_description_index));
    indent();
    printf("default_sample_description_index: %u\n", default_sample_description_index);

    uint32_t default_sample_duration;
    MOV_CHECK(read_uint32(&default_sample_duration));
    indent();
    printf("default_sample_duration: ");
    dump_uint32(default_sample_duration, true);
    printf("\n");

    uint32_t default_sample_size;
    MOV_CHECK(read_uint32(&default_sample_size));
    indent();
    printf("default_sample_size: ");
    dump_uint32(default_sample_size, true);
    printf("\n");

    uint32_t default_sample_flags;
    MOV_CHECK(read_uint32(&default_sample_flags));
    indent();
    printf("default_sample_flags: 0x%08x (", default_sample_flags);
    dump_fragment_sample_flags(default_sample_flags);
    printf(")\n");
}

static void dump_mvex_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'m','e','h','d'}, dump_mehd_atom},
        {{'t','r','e','x'}, dump_trex_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_moov_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'m','v','h','d'}, dump_mvhd_atom},
        {{'t','r','a','k'}, dump_trak_atom},
        {{'m','e','t','a'}, dump_meta_atom},
        {{'u','d','t','a'}, dump_udta_atom},
        {{'m','v','e','x'}, dump_mvex_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_sidx_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t reference_id;
    MOV_CHECK(read_uint32(&reference_id));
    indent();
    printf("reference_id: %u\n", reference_id);

    uint32_t timescale;
    MOV_CHECK(read_uint32(&timescale));
    indent();
    printf("timescale: %u\n", timescale);

    if (version == 0x00) {
        uint32_t earliest_pres_time;
        MOV_CHECK(read_uint32(&earliest_pres_time));
        indent();
        printf("earliest_presentation_time: %u\n", earliest_pres_time);

        uint32_t first_offset;
        MOV_CHECK(read_uint32(&first_offset));
        indent();
        printf("first_offset: %u\n", first_offset);
    } else {
        uint64_t earliest_pres_time;
        MOV_CHECK(read_uint64(&earliest_pres_time));
        indent();
        printf("earliest_presentation_time: %" PRIu64 "\n", earliest_pres_time);

        uint64_t first_offset;
        MOV_CHECK(read_uint64(&first_offset));
        indent();
        printf("first_offset: %" PRIu64 "\n", first_offset);
    }

    uint16_t reserved_uint16;
    MOV_CHECK(read_uint16(&reserved_uint16));
    indent();
    printf("reserved: ");
    dump_uint16(reserved_uint16, true);
    printf("\n");

    uint16_t num_entries;
    MOV_CHECK(read_uint16(&num_entries));
    indent();
    printf("references (");
    dump_uint16(num_entries, false);
    printf("):\n");

    indent(4);
    if (num_entries < 0xff)
        printf("%2s", "i");
    else
        printf("%4s", "i");
    printf("%10s%12s%14s%16s%10s%16s\n",
           "ref_type", "ref_size",  "subseg_dur", "start_with_sap", "sap_type", "sap_delta_time");

    uint16_t i;
    for (i = 0; i < num_entries; i++) {
        uint32_t reference_word;
        MOV_CHECK(read_uint32(&reference_word));
        uint32_t subsegment_duration;
        MOV_CHECK(read_uint32(&subsegment_duration));
        uint32_t sap_word;
        MOV_CHECK(read_uint32(&sap_word));

        indent(4);
        dump_uint16_index(num_entries, i);

        if (reference_word & 0x80000000)
            printf("%10s", "sidx");
        else
            printf("%10s", "media");

        printf("%*c", 2, ' ');
        dump_uint32(reference_word & 0x7fffffff, true);

        printf("%*c", 4, ' ');
        dump_uint32(subsegment_duration, true);

        if (sap_word & 0x80000000)
            printf("%16s", "true");
        else
            printf("%16s", "false");

        printf("%*c", 7, ' ');
        dump_uint8((sap_word >> 28) & ((1 << 3) - 1), false);

        printf("%*c", 6, ' ');
        dump_uint32(sap_word & ((1 << 28) - 1), false);
        printf("\n");
    }
}

static void dump_mfhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t sequence_number;
    MOV_CHECK(read_uint32(&sequence_number));
    indent();
    printf("sequence_number: %u\n", sequence_number);
}

static void dump_tfhd_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t track_id;
    MOV_CHECK(read_uint32(&track_id));
    indent();
    printf("track_id: %u\n", track_id);

    if (flags & 0x000001) {
        uint64_t base_data_offset;
        MOV_CHECK(read_uint64(&base_data_offset));
        indent();
        printf("base_data_offset: %" PRIu64 "\n", base_data_offset);
    }
    if (flags & 0x000002) {
        uint32_t sample_description_index;
        MOV_CHECK(read_uint32(&sample_description_index));
        indent();
        printf("sample_description_index: %u\n", sample_description_index);
    }
    if (flags & 0x000008) {
        uint32_t default_sample_duration;
        MOV_CHECK(read_uint32(&default_sample_duration));
        indent();
        printf("default_sample_duration: %u\n", default_sample_duration);
    }
    if (flags & 0x000010) {
        uint32_t default_sample_size;
        MOV_CHECK(read_uint32(&default_sample_size));
        indent();
        printf("default_sample_size: %u\n", default_sample_size);
    }
    if (flags & 0x000020) {
        uint32_t default_sample_flags;
        MOV_CHECK(read_uint32(&default_sample_flags));
        indent();
        printf("default_sample_flags: 0x%08x (", default_sample_flags);
        dump_fragment_sample_flags(default_sample_flags);
        printf(")\n");
    }
}

static void dump_trun_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t num_entries;
    MOV_CHECK(read_uint32(&num_entries));

    if (flags & 0x000001) {
        int32_t data_offset;
        MOV_CHECK(read_int32(&data_offset));
        indent();
        printf("data_offset: %d\n", data_offset);
    }
    if (flags & 0x000004) {
        uint32_t first_sample_flags;
        MOV_CHECK(read_uint32(&first_sample_flags));
        indent();
        printf("first_sample_flags: 0x%08x (", first_sample_flags);
        dump_fragment_sample_flags(first_sample_flags);
        printf(")\n");
    }

    if (num_entries > 0) {
        indent();
        printf("samples (");
        dump_uint32(num_entries, false);
        printf("):\n");

        indent(4);
        if (num_entries < 0xffff)
            printf("%4s", "i");
        else if (num_entries < 0xffffff)
            printf("%6s", "i");
        else
            printf("%8s", "i");
        printf("%12s%12s%12s%12s\n",
               "duration", "size",  "flags", "ct_offset");

        uint32_t i;
        for (i = 0; i < num_entries; i++) {
            indent(4);
            dump_uint32_index(num_entries, i);

            if (flags & 0x000100) {
                uint32_t sample_duration;
                MOV_CHECK(read_uint32(&sample_duration));
                printf("%*c", 2, ' ');
                dump_uint32(sample_duration, true);
            } else {
                printf("%12s", "x");
            }
            if (flags & 0x000200) {
                uint32_t sample_size;
                MOV_CHECK(read_uint32(&sample_size));
                printf("%*c", 2, ' ');
                dump_uint32(sample_size, true);
            } else {
                printf("%12s", "x");
            }
            if (flags & 0x000400) {
                uint32_t sample_flags;
                MOV_CHECK(read_uint32(&sample_flags));
                printf("%*c", 2, ' ');
                dump_uint32(sample_flags, true);
            } else {
                printf("%12s", "x");
            }
            if (flags & 0x000800) {
                if (version == 0) {
                    uint32_t composition_time_offset;
                    MOV_CHECK(read_uint32(&composition_time_offset));
                    printf("%*c", 2, ' ');
                    dump_uint32(composition_time_offset, false);
                } else {
                    int32_t composition_time_offset;
                    MOV_CHECK(read_int32(&composition_time_offset));
                    printf("%*c", 2, ' ');
                    dump_int32(composition_time_offset);
                }
            } else {
                printf("%12s", "x");
            }
            printf("\n");
        }
    }
}

static void dump_tfdt_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    if (version == 0) {
        uint32_t base_media_decode_time;
        MOV_CHECK(read_uint32(&base_media_decode_time));
        indent();
        printf("base_media_decode_time: ");
        dump_uint32(base_media_decode_time, true);
        printf("\n");
    } else {
        uint64_t base_media_decode_time;
        MOV_CHECK(read_uint64(&base_media_decode_time));
        indent();
        printf("base_media_decode_time: ");
        dump_uint64(base_media_decode_time, true);
        printf("\n");
    }
}

static void dump_traf_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'t','f','h','d'}, dump_tfhd_atom},
        {{'t','r','u','n'}, dump_trun_atom},
        {{'t','f','d','t'}, dump_tfdt_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_moof_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'m','f','h','d'}, dump_mfhd_atom},
        {{'t','r','a','f'}, dump_traf_atom},
    };

    dump_container_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}

static void dump_ssix_atom()
{
    uint8_t version;
    uint32_t flags;
    dump_full_atom_header(&version, &flags);


    uint32_t sub_seg_count;
    MOV_CHECK(read_uint32(&sub_seg_count));
    indent();
    printf("sub_segments (");
    dump_uint32(sub_seg_count, false);
    printf("):\n");

    uint32_t i;
    for (i = 0; i < sub_seg_count; i++) {
        indent(4);
        dump_uint32_index(sub_seg_count, i);

        uint32_t ranges_count;
        MOV_CHECK(read_uint32(&ranges_count));
        printf(": ranges (");
        dump_uint32(ranges_count, false);
        printf("):\n");

        indent(4);
        if (ranges_count < 0xffff)
            printf("%4s", "i");
        else if (ranges_count < 0xffffff)
            printf("%6s", "i");
        else
            printf("%8s", "i");
        printf("%8s%12s\n",
               "level", "range_size");

        uint32_t j;
        for (j = 0; j < ranges_count; j++) {
            indent(4);
            dump_uint32_index(ranges_count, j);

            uint8_t level;
            MOV_CHECK(read_uint8(&level));
            printf("%*c", 4, ' ');
            dump_uint8(level, true);

            uint32_t range_size;
            MOV_CHECK(read_uint24(&range_size));
            printf("%*c", 2, ' ');
            dump_uint32(range_size, true);
            printf("\n");
        }
    }
}

static void dump_top_atom()
{
    static const DumpFuncMap dump_func_map[] =
    {
        {{'f','t','y','p'}, dump_ftyp_styp_atom},
        {{'s','t','y','p'}, dump_ftyp_styp_atom},
        {{'m','d','a','t'}, dump_mdat_atom},
        {{'f','r','e','e'}, dump_free_atom},
        {{'s','k','i','p'}, dump_skip_atom},
        {{'m','o','o','v'}, dump_moov_atom},
        {{'s','i','d','x'}, dump_sidx_atom},
        {{'m','o','o','f'}, dump_moof_atom},
        {{'s','s','i','x'}, dump_ssix_atom},
    };

    dump_child_atom(dump_func_map, DUMP_FUNC_MAP_SIZE);
}


static void dump_file()
{
    while (true) {
        push_atom();

        if (!read_atom_start())
            break;

        dump_top_atom();

        pop_atom();
    }
}

static void usage(const char *cmd)
{
    fprintf(stderr, "Usage: %s [options] <quicktime filename>\n", cmd);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, " -h | --help       Print this usage message and exit\n");
    fprintf(stderr, "  --avcc <fname>   Write SPS and PPS NAL units in the 'avcC' box to <fname> file\n");
    fprintf(stderr, "                   The NAL units are prefixed by a length word with size defined in the 'avcC' box\n");
}

int main(int argc, const char **argv)
{
    const char *filename;
    int cmdln_index;

    // parse commandline arguments

    for (cmdln_index = 1; cmdln_index < argc; cmdln_index++) {
        if (strcmp(argv[cmdln_index], "-h") == 0 ||
            strcmp(argv[cmdln_index], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[cmdln_index], "--avcc") == 0)
        {
            if (cmdln_index + 1 >= argc)
            {
                usage(argv[0]);
                fprintf(stderr, "Missing argument for option '%s'\n", argv[cmdln_index]);
                return 1;
            }
            g_avcc_filename = argv[cmdln_index + 1];
            cmdln_index++;
        }
        else
        {
            break;
        }
    }

    if (cmdln_index + 1 < argc) {
        usage(argv[0]);
        fprintf(stderr, "Unknown argument '%s'\n", argv[cmdln_index]);
        return 1;
    }
    if (cmdln_index + 1 > argc) {
        if (argc == 1) {
            usage(argv[0]);
            return 0;
        }
        usage(argv[0]);
        fprintf(stderr, "Missing quicktime filename\n");
        return 1;
    }

    filename = argv[cmdln_index];


    // open file

    g_mov_file = fopen(filename, "rb");
    if (!g_mov_file) {
        fprintf(stderr, "Failed to open quicktime file '%s': %s\n", filename, strerror(errno));
        return 1;
    }


    // dump file

    try
    {
        dump_file();
    }
    catch (exception &ex)
    {
        fprintf(stderr, "Exception: %s\n", ex.what());
        return 1;
    }
    catch (...)
    {
        fprintf(stderr, "Unexpected exception\n");
        return 1;
    }

    if (g_mov_file)
        fclose(g_mov_file);
    if (g_avcc_file)
        fclose(g_avcc_file);

    return 0;
}

