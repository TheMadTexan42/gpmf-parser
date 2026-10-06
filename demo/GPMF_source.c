/* Shared demo input source. Licensed under Apache-2.0 or MIT. */
#include "GPMF_source.h"
#include "GPMF_mp4reader.h"
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>

int GPMF_SourceOpen(GPMF_source *source, const char *path, int raw)
{
#if defined(_WIN32) || defined(_WINDOWS)
    struct _stat64 info;
#else
    struct stat info;
#endif
    memset(source, 0, sizeof(*source));
    source->raw = raw;
    if (raw) {
        if (GPMF_RawOpen(&source->reader, path)) return 1;
        snprintf(source->error, sizeof(source->error), "%s", source->reader.error);
    } else {
        /* The legacy MP4 reader does not check stat() failure itself. */
#if defined(_WIN32) || defined(_WINDOWS)
        if (_stat64(path, &info) != 0) {
#else
        if (stat(path, &info) != 0) {
#endif
            snprintf(source->error, sizeof(source->error), "cannot stat input file");
            return 0;
        }
        source->mp4 = OpenMP4Source((char *)path, MOV_GPMF_TRAK_TYPE, MOV_GPMF_TRAK_SUBTYPE, 0);
        if (source->mp4 && GetNumberPayloads(source->mp4)) return 1;
        snprintf(source->error, sizeof(source->error), "invalid MP4/MOV or no GPMF payloads");
        if (source->mp4) CloseSource(source->mp4);
        source->mp4 = 0;
    }
    return 0;
}

int GPMF_SourceNext(GPMF_source *source)
{
    uint32_t *buffer;
    GPMF_ERR error;
    /* Retain the last MP4 stream for the demo's sample-rate diagnostics. */
    if (!source->raw && source->count >= GetNumberPayloads(source->mp4)) return 0;
    GPMF_Free(&source->stream);
    memset(&source->stream, 0, sizeof(source->stream));
    source->has_time = 0;
    source->in = source->out = 0;
    source->index = source->count;
    if (source->raw) {
        int result = GPMF_RawNext(&source->reader);
        source->offset = source->reader.offset;
        if (result < 0) {
            snprintf(source->error, sizeof(source->error), "raw record at byte %" PRIu64 ": %s",
                     source->offset, source->reader.error);
        }
        if (result != 1) return result;
        buffer = source->reader.buffer;
        source->size = source->reader.size;
    } else {
        if (source->index >= GetNumberPayloads(source->mp4)) return 0;
        source->size = GetPayloadSize(source->mp4, (uint32_t)source->index);
        source->resource = GetPayloadResource(source->mp4, source->resource, source->size);
        buffer = GetPayload(source->mp4, source->resource, (uint32_t)source->index);
        if (!buffer || !source->size) {
            snprintf(source->error, sizeof(source->error), "cannot read MP4 payload %" PRIu64, source->index);
            return -1;
        }
        source->has_time = GetPayloadTime(source->mp4, (uint32_t)source->index,
                                         &source->in, &source->out) == MP4_ERROR_OK;
    }
    error = GPMF_PrepareBuffer(&source->stream, buffer, source->size);
    if (error != GPMF_OK) {
        snprintf(source->error, sizeof(source->error), "invalid GPMF in %s %" PRIu64 " (parser error %u)",
                 source->raw ? "raw record" : "MP4 payload", source->index, error);
        return -1;
    }
    source->count++;
    return 1;
}

void GPMF_SourceClose(GPMF_source *source)
{
    GPMF_Free(&source->stream);
    if (source->raw) GPMF_RawClose(&source->reader);
    else {
        if (source->resource) FreePayloadResource(source->mp4, source->resource);
        if (source->mp4) CloseSource(source->mp4);
    }
    source->mp4 = source->resource = 0;
}
