/* Binary GPMF record reader. Licensed under Apache-2.0 or MIT. */
#include "GPMF_rawreader.h"
#include <stdlib.h>
#include <string.h>

static uint32_t value_size(const unsigned char *header)
{
    return (uint32_t)header[5] * (((uint32_t)header[6] << 8) | header[7]);
}

static int valid_key(const unsigned char *key)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        if (!((key[i] >= 'A' && key[i] <= 'Z') ||
              (key[i] >= 'a' && key[i] <= 'z') ||
              (key[i] >= '0' && key[i] <= '9') || key[i] == ' '))
            return 0;
    return 1;
}

static int check_records(const unsigned char *data, uint32_t size, unsigned depth)
{
    uint32_t pos = 0;
    if (depth >= GPMF_NEST_LIMIT) return 0;
    while (pos < size) {
        const unsigned char *h = data + pos;
        uint32_t length, stored;
        /* Zero words are permitted as padding inside a nest, never at top level. */
        if (depth && size - pos >= 4 && !memcmp(h, "\0\0\0\0", 4)) {
            pos += 4;
            continue;
        }
        if (size - pos < 8 || !valid_key(h) || !h[5]) return 0;
        if (!depth && (memcmp(h, "DEVC", 4) || h[4] != 0)) return 0;
        length = value_size(h);
        stored = (length + 3u) & ~3u;
        if (stored > size - pos - 8) return 0;
        if (h[4] == GPMF_TYPE_NEST) {
            if ((length & 3u) || !length ||
                !check_records(h + 8, length, depth + 1)) return 0;
        } else if (h[4] == GPMF_TYPE_COMPRESSED) {
            /* The compressed value starts with its original type/size/count. */
            if (length < 4 || !h[9] ||
                (h[8] != GPMF_TYPE_COMPLEX && !GPMF_SizeofType((GPMF_SampleType)h[8])))
                return 0;
        } else if (h[4] != GPMF_TYPE_COMPLEX) {
            uint32_t type_size = GPMF_SizeofType((GPMF_SampleType)h[4]);
            if (type_size && h[5] % type_size) return 0;
        }
        pos += 8 + stored;
    }
    return pos == size;
}

GPMF_ERR GPMF_PrepareBuffer(GPMF_stream *stream, uint32_t *buffer, uint32_t size)
{
    GPMF_ERR error;
    if (!stream || !buffer || size < 8 || (size & 3u) ||
        !check_records((const unsigned char *)buffer, size, 0))
        return GPMF_ERROR_BAD_STRUCTURE;
    error = GPMF_Init(stream, buffer, size);
    if (error != GPMF_OK) return error;
    if (stream->buffer_size_longs != size / 4) return GPMF_ERROR_BAD_STRUCTURE;
    error = GPMF_Validate(stream, GPMF_RECURSE_LEVELS);
    GPMF_ResetState(stream);
    return error;
}

int GPMF_RawOpen(GPMF_rawreader *reader, const char *path)
{
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (!reader->file) {
        snprintf(reader->error, sizeof(reader->error), "cannot open input file");
        return 0;
    }
    return 1;
}

int GPMF_RawNext(GPMF_rawreader *reader)
{
    unsigned char header[8];
    size_t got;
    uint32_t length;
    free(reader->buffer);
    reader->buffer = NULL;
    reader->size = 0;
    reader->offset = reader->next_offset;
    got = fread(header, 1, sizeof(header), reader->file);
    if (!got && !ferror(reader->file) && reader->records) return 0;
    if (got != sizeof(header)) {
        snprintf(reader->error, sizeof(reader->error), "%s",
                 ferror(reader->file) ? "input read failed" :
                 (!got ? "empty GPMF input" : "truncated record header or trailing bytes"));
        return -1;
    }
    if (memcmp(header, "DEVC", 4) || header[4] || !header[5]) {
        snprintf(reader->error, sizeof(reader->error), "expected a nested DEVC record");
        return -1;
    }
    length = value_size(header);
    if (!length || (length & 3u)) {
        snprintf(reader->error, sizeof(reader->error), "invalid DEVC value length");
        return -1;
    }
    /* An individual KLV value is bounded by 255 * 65535 bytes. */
    reader->size = length + 8;
    if (UINT64_MAX - reader->next_offset < reader->size) {
        snprintf(reader->error, sizeof(reader->error), "file offset overflow");
        return -1;
    }
    reader->buffer = (uint32_t *)malloc(reader->size);
    if (!reader->buffer) {
        snprintf(reader->error, sizeof(reader->error), "record allocation failed");
        return -1;
    }
    memcpy(reader->buffer, header, 8);
    if (fread((unsigned char *)reader->buffer + 8, 1, length, reader->file) != length) {
        snprintf(reader->error, sizeof(reader->error), "%s",
                 ferror(reader->file) ? "input read failed" : "truncated DEVC value");
        return -1;
    }
    reader->next_offset += reader->size;
    reader->records++;
    return 1;
}

void GPMF_RawClose(GPMF_rawreader *reader)
{
    if (reader->file) fclose(reader->file);
    free(reader->buffer);
    reader->file = NULL;
    reader->buffer = NULL;
}
