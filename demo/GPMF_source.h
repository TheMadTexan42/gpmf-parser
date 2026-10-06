/* Shared demo input source. Licensed under Apache-2.0 or MIT. */
#ifndef GPMF_SOURCE_H
#define GPMF_SOURCE_H
#include "GPMF_rawreader.h"

typedef struct GPMF_source {
    int raw;
    GPMF_rawreader reader;
    size_t mp4;
    size_t resource;
    uint64_t index;
    uint64_t offset;
    uint64_t count;
    uint32_t size;
    int has_time;
    double in, out;
    GPMF_stream stream;
    char error[200];
} GPMF_source;

int GPMF_SourceOpen(GPMF_source *source, const char *path, int raw);
/* 1 = validated buffer, 0 = EOF, -1 = error. */
int GPMF_SourceNext(GPMF_source *source);
void GPMF_SourceClose(GPMF_source *source);
#endif
