/* Binary GPMF record reader. Licensed under Apache-2.0 or MIT. */
#ifndef GPMF_RAWREADER_H
#define GPMF_RAWREADER_H

#include <stdio.h>
#include "../GPMF_parser.h"

typedef struct GPMF_rawreader {
    FILE *file;
    uint32_t *buffer;
    uint32_t size;
    uint64_t offset;
    uint64_t next_offset;
    uint64_t records;
    char error[160];
} GPMF_rawreader;

int GPMF_RawOpen(GPMF_rawreader *reader, const char *path);
/* 1 = complete record, 0 = EOF, -1 = error. Buffer lives until next read/close. */
int GPMF_RawNext(GPMF_rawreader *reader);
void GPMF_RawClose(GPMF_rawreader *reader);
/* Check bounds/depth before invoking the parser, including full consumption. */
GPMF_ERR GPMF_PrepareBuffer(GPMF_stream *stream, uint32_t *buffer, uint32_t size);

#endif
