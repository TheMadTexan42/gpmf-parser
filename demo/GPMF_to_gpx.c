/* Export GPS metadata using GPS-recorded UTC. Licensed under Apache-2.0 or MIT. */
#include "GPMF_source.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <ctype.h>

static int same_path(const char *input, const char *output)
{
#ifdef _WIN32
    char *a = _fullpath(NULL, input, 0), *b = _fullpath(NULL, output, 0);
    int equal = !strcmp(input, output) || (a && b && !_stricmp(a, b));
    free(a);
    free(b);
    return equal;
#else
    char *a = realpath(input, NULL), *b = realpath(output, NULL);
    int equal = !strcmp(input, output) || (a && b && !strcmp(a, b));
    free(a);
    free(b);
    return equal;
#endif
}

typedef struct GpxStats {
    uint64_t points, untimed, rejected_fix, invalid, duplicate, regression;
    int have_previous;
    int64_t previous;
} GpxStats;

typedef enum FixRequirement {
    FIX_ALL,
    FIX_2D,
    FIX_3D
} FixRequirement;

static int accepts_fix(FixRequirement requirement, int fix)
{
    return requirement == FIX_ALL || fix == 3 || (requirement == FIX_2D && fix == 2);
}

static int leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int date_string(int64_t timestamp, char *text, size_t size)
{
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days, day_time;
    int year = 2000, month = 1;
    if (timestamp < 0) return 0;
    days = timestamp / INT64_C(86400000000);
    day_time = timestamp % INT64_C(86400000000);
    while (year <= 9999 && days >= 365 + leap(year)) {
        days -= 365 + leap(year);
        year++;
    }
    if (year > 9999) return 0;
    while (month < 12) {
        int length = month_days[month-1] + (month == 2 && leap(year));
        if (days < length) break;
        days -= length;
        month++;
    }
    snprintf(text, size, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ", year, month,
             (int)days + 1, (int)(day_time / INT64_C(3600000000)),
             (int)(day_time / INT64_C(60000000) % 60),
             (int)(day_time / 1000000 % 60), (int)(day_time % 1000000));
    return 1;
}

static int gps9_time(int32_t days, int32_t seconds, double day_scale,
                     double second_scale, int64_t *timestamp)
{
    int64_t ds, ss, whole_days, micros;
    char check[40];
    if (!isfinite(day_scale) || !isfinite(second_scale) || day_scale <= 0 ||
        second_scale <= 0 || day_scale > INT32_MAX || second_scale > INT32_MAX ||
        floor(day_scale) != day_scale || floor(second_scale) != second_scale)
        return 0;
    ds = (int64_t)day_scale;
    ss = (int64_t)second_scale;
    if (days < 0 || days % ds || seconds < 0 || (int64_t)seconds >= 86400 * ss) return 0;
    whole_days = days / ds;
    /* Bound to XML's four-digit years before multiplying. */
    if (whole_days > INT64_C(2921939)) return 0;
    micros = (int64_t)seconds * 1000000 / ss;
    *timestamp = whole_days * INT64_C(86400000000) + micros;
    return date_string(*timestamp, check, sizeof(check));
}

static int previous_field(GPMF_stream *sample, uint32_t key, GPMF_stream *field)
{
    GPMF_CopyState(sample, field);
    return GPMF_FindPrev(field, key, GPMF_CURRENT_LEVEL) == GPMF_OK;
}

static int read_scales(GPMF_stream *sample, double scales[9])
{
    GPMF_stream field;
    uint32_t count, i, type_size;
    unsigned char data[9 * 8];
    for (i = 0; i < 9; ++i) scales[i] = 1;
    if (!previous_field(sample, GPMF_KEY_SCALE, &field)) return 1;
    type_size = GPMF_SizeofType(GPMF_Type(&field));
    if (!type_size || GPMF_FormattedDataSize(&field) % type_size) return 0;
    count = GPMF_FormattedDataSize(&field) / type_size;
    if (count != 1 && count != 9) return 0;
    if (GPMF_FormattedDataSize(&field) > sizeof(data) ||
        GPMF_FormattedData(&field, data, sizeof(data), 0, GPMF_Repeat(&field)) != GPMF_OK) return 0;
    for (i = 0; i < count; ++i) {
        switch (GPMF_Type(&field)) {
        case GPMF_TYPE_SIGNED_LONG: { int32_t v; memcpy(&v, data + i*4, 4); scales[i] = v; break; }
        case GPMF_TYPE_UNSIGNED_LONG: { uint32_t v; memcpy(&v, data + i*4, 4); scales[i] = v; break; }
        case GPMF_TYPE_SIGNED_SHORT: { int16_t v; memcpy(&v, data + i*2, 2); scales[i] = v; break; }
        case GPMF_TYPE_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, data + i*2, 2); scales[i] = v; break; }
        default: return 0;
        }
        if (scales[i] <= 0) return 0;
    }
    if (count == 1) for (i = 1; i < 9; ++i) scales[i] = scales[0];
    return 1;
}

/* GPSU anchors the first GPS5 sample. Later samples remain untimed. */
static int gpsu_time(GPMF_stream *sample, int64_t *timestamp)
{
    GPMF_stream field;
    const unsigned char *data;
    int parts[7], i, year, month, day;
    int64_t days = 0;
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!previous_field(sample, STR2FOURCC("GPSU"), &field) || GPMF_RawDataSize(&field) != 16)
        return 0;
    data = (const unsigned char *)GPMF_RawData(&field);
    if (data[12] != '.') return 0;
    for (i = 0; i < 16; ++i) if (i != 12 && !isdigit(data[i])) return 0;
    for (i = 0; i < 6; ++i) parts[i] = (data[i*2]-'0')*10 + data[i*2+1]-'0';
    parts[6] = (data[13]-'0')*100 + (data[14]-'0')*10 + data[15]-'0';
    year = 2000 + parts[0]; month = parts[1]; day = parts[2];
    if (month < 1 || month > 12 || day < 1 ||
        day > lengths[month-1] + (month == 2 && leap(year)) ||
        parts[3] > 23 || parts[4] > 59 || parts[5] > 59) return 0;
    for (i = 2000; i < year; ++i) days += 365 + leap(i);
    for (i = 1; i < month; ++i) days += lengths[i-1] + (i == 2 && leap(year));
    days += day - 1;
    *timestamp = days * INT64_C(86400000000) +
        (parts[3]*3600 + parts[4]*60 + parts[5])*INT64_C(1000000) + parts[6]*1000;
    return 1;
}

static int write_point(FILE *file, const double *values, int timed,
                       int64_t timestamp, GpxStats *stats)
{
    char text[40];
    if (!isfinite(values[0]) || !isfinite(values[1]) || !isfinite(values[2]) ||
        fabs(values[0]) > 90 || fabs(values[1]) > 180) {
        stats->invalid++;
        return 1;
    }
    if (timed && !date_string(timestamp, text, sizeof(text))) timed = 0;
    if (fprintf(file, "    <trkpt lat=\"%.9f\" lon=\"%.9f\"><ele>%.3f</ele>",
                values[0], values[1], values[2]) < 0) return 0;
    if (timed) {
        if (fprintf(file, "<time>%s</time>", text) < 0) return 0;
        if (stats->have_previous) {
            stats->duplicate += timestamp == stats->previous;
            stats->regression += timestamp < stats->previous;
        }
        stats->previous = timestamp;
        stats->have_previous = 1;
    } else stats->untimed++;
    stats->points++;
    return fprintf(file, "</trkpt>\n") >= 0;
}

static int export_buffer(FILE *file, GPMF_stream *stream, GpxStats *stats,
                         FixRequirement requirement)
{
    GPMF_ERR next;
    do {
        uint32_t key = GPMF_Key(stream);
        if (key == (uint32_t)STR2FOURCC("GPS9") || key == (uint32_t)STR2FOURCC("GPS5")) {
            uint32_t count = GPMF_Repeat(stream), elements = GPMF_ElementsInStruct(stream), i;
            double *values, scales[9];
            int gps9 = key == (uint32_t)STR2FOURCC("GPS9"), has_anchor = 0, fix = -1;
            int64_t anchor = 0;
            GPMF_stream field;
            if ((gps9 && (elements != 9 || GPMF_StructSize(stream) != 32)) ||
                (!gps9 && elements != 5)) return 0;
            if (gps9) {
                char type[16];
                uint32_t type_length = sizeof(type);
                if (!previous_field(stream, GPMF_KEY_TYPE, &field) ||
                    GPMF_ExpandComplexTYPE((char *)GPMF_RawData(&field), GPMF_RawDataSize(&field),
                                          type, &type_length) != GPMF_OK ||
                    strcmp(type, "lllllllSS") || !read_scales(stream, scales))
                    return 0;
            } else {
                has_anchor = gpsu_time(stream, &anchor);
                if (previous_field(stream, STR2FOURCC("GPSF"), &field)) {
                    uint32_t v;
                    if (GPMF_Type(&field) != GPMF_TYPE_UNSIGNED_LONG ||
                        GPMF_FormattedData(&field, &v, sizeof(v), 0, 1) != GPMF_OK) return 0;
                    fix = v == 2 ? 2 : v == 3 ? 3 : 0;
                }
            }
            if (!count) goto advance;
            values = (double *)malloc(GPMF_ScaledDataSize(stream, GPMF_TYPE_DOUBLE));
            if (!values) return 0;
            if (GPMF_ScaledData(stream, values, GPMF_ScaledDataSize(stream, GPMF_TYPE_DOUBLE),
                                0, count, GPMF_TYPE_DOUBLE) != GPMF_OK) {
                free(values);
                return 0;
            }
            for (i = 0; i < count; ++i) {
                int timed = has_anchor && i == 0;
                int64_t timestamp = anchor;
                if (gps9) {
                    unsigned char formatted[32];
                    int32_t days, seconds;
                    uint16_t recorded_fix;
                    if (GPMF_FormattedData(stream, formatted, sizeof(formatted), i, 1) != GPMF_OK) {
                        free(values); return 0;
                    }
                    memcpy(&days, formatted + 20, 4);
                    memcpy(&seconds, formatted + 24, 4);
                    memcpy(&recorded_fix, formatted + 30, 2);
                    fix = recorded_fix / scales[8] == 2 ? 2 :
                          recorded_fix / scales[8] == 3 ? 3 : 0;
                    timed = gps9_time(days, seconds, scales[5], scales[6], &timestamp);
                }
                if (!accepts_fix(requirement, fix)) { stats->rejected_fix++; continue; }
                if (!write_point(file, values + (size_t)i * elements, timed, timestamp, stats)) {
                    free(values); return 0;
                }
            }
            free(values);
        }
advance:
        next = GPMF_Next(stream, GPMF_RECURSE_LEVELS);
    } while (next == GPMF_OK);
    return next == GPMF_ERROR_BUFFER_END;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    char *default_output = NULL;
    int raw = 0, result, status = EXIT_FAILURE, i;
    FILE *spool = NULL, *destination = NULL;
    GPMF_source source;
    GpxStats stats = {0};
    FixRequirement requirement = FIX_2D;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--raw")) raw = 1;
        else if (!strcmp(argv[i], "--fix") || !strncmp(argv[i], "--fix=", 6)) {
            const char *value;
            if (argv[i][5] == '=') value = argv[i] + 6;
            else {
                if (++i == argc) { fprintf(stderr, "--fix requires 3d, 2d, or all\n"); return status; }
                value = argv[i];
            }
            if (!strcmp(value, "3d")) requirement = FIX_3D;
            else if (!strcmp(value, "2d")) requirement = FIX_2D;
            else if (!strcmp(value, "all")) requirement = FIX_ALL;
            else { fprintf(stderr, "invalid --fix value: %s (use 3d, 2d, or all)\n", value); return status; }
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s <input.mp4|input.gpmf> [output.gpx] [--raw] [--fix 3d|2d|all]\n"
                   "--fix 3d: require 3D; 2d: require 2D or 3D (default); all: ignore fix status.\n"
                   "GPX times use recorded GPS UTC only; points without GPS time omit <time>.\n", argv[0]);
            return EXIT_SUCCESS;
        } else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); return status; }
        else if (!input) input = argv[i];
        else if (!output) output = argv[i];
        else { fprintf(stderr, "too many paths\n"); return status; }
    }
    if (!input) { fprintf(stderr, "usage: %s <input> [output.gpx] [--raw] [--fix 3d|2d|all]\n", argv[0]); return status; }
    if (!output) {
        const char *dot = strrchr(input, '.');
        const char *slash = strrchr(input, '/'), *backslash = strrchr(input, '\\');
        size_t length;
        if ((slash && dot && dot < slash) || (backslash && dot && dot < backslash)) dot = NULL;
        length = dot ? (size_t)(dot-input) : strlen(input);
        default_output = (char *)malloc(length + 5);
        if (!default_output) return status;
        memcpy(default_output, input, length);
        memcpy(default_output + length, ".gpx", 5);
        output = default_output;
    }
    if (same_path(input, output)) { fprintf(stderr, "output must differ from input\n"); free(default_output); return status; }
    if (!GPMF_SourceOpen(&source, input, raw)) {
        fprintf(stderr, "error: %s: %s\n", input, source.error);
        free(default_output); return status;
    }
    /* Validate/decode the entire input before opening the requested output. */
    spool = tmpfile();
    if (!spool) { fprintf(stderr, "cannot create temporary GPX stream\n"); goto cleanup; }
    while ((result = GPMF_SourceNext(&source)) == 1) {
        if (!export_buffer(spool, &source.stream, &stats, requirement)) {
            fprintf(stderr, "GPS decoding or output failed at record %" PRIu64 "\n", source.index);
            goto cleanup;
        }
    }
    if (result < 0) { fprintf(stderr, "error: %s\n", source.error); goto cleanup; }
    if (!stats.points) { fprintf(stderr, "no valid GPS positions found\n"); goto cleanup; }
    if (fflush(spool) || fseek(spool, 0, SEEK_SET)) { fprintf(stderr, "temporary output failed\n"); goto cleanup; }
    destination = fopen(output, "wb");
    if (!destination) { fprintf(stderr, "cannot open output: %s\n", output); goto cleanup; }
    if (fprintf(destination, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<gpx version=\"1.1\" creator=\"gpmf-parser\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
                "  <trk><trkseg>\n") < 0) goto cleanup;
    for (;;) {
        char data[8192];
        size_t bytes = fread(data, 1, sizeof(data), spool);
        if (bytes && fwrite(data, 1, bytes, destination) != bytes) goto cleanup;
        if (bytes < sizeof(data)) { if (ferror(spool)) goto cleanup; break; }
    }
    if (fprintf(destination, "  </trkseg></trk>\n</gpx>\n") < 0) goto cleanup;
    result = fclose(destination); destination = NULL;
    if (result) { fprintf(stderr, "output close failed\n"); goto cleanup; }
    fprintf(stderr, "Exported %" PRIu64 " points (%" PRIu64 " untimed); skipped %" PRIu64
            " below requested fix level and %" PRIu64 " invalid positions.\n", stats.points, stats.untimed,
            stats.rejected_fix, stats.invalid);
    if (stats.duplicate || stats.regression)
        fprintf(stderr, "Recorded timestamps preserved: %" PRIu64 " duplicates, %" PRIu64 " regressions.\n",
                stats.duplicate, stats.regression);
    status = EXIT_SUCCESS;
cleanup:
    if (destination) { fprintf(stderr, "output write failed\n"); fclose(destination); }
    if (spool) fclose(spool);
    GPMF_SourceClose(&source);
    free(default_output);
    return status;
}
