# Plan: consume extracted GPMF files directly

## Objective

Allow the demo executable to decode a binary GPMF file directly, including a
single extracted payload or a sequence of complete top-level `DEVC` records.
Reuse the existing parser for traversal, scaling, complex types, and compressed
data. Preserve existing MP4/MOV decoding and diagnostic timing behavior.

GPX export from either raw GPMF or MP4/MOV must use timestamps recorded by the
GPS device. Video/container timing and camera-relative timestamps must not
determine GPX point times.

This input mode expects the original binary records, not JSON, hexadecimal text,
or an MP4 `mdat` atom containing unrelated media.

## Reference sample and current behavior

The user-provided sample is
`C:\Video to Edit\Oct 3 Ride\Max2\GS010012.gpmf`.
Prior binary inspection found:

- 6,693,736 bytes, beginning with `DEVC` and identifying the device as MAX2.
- 448 consecutive top-level `DEVC` records, with nested record boundaries
  covering the entire file without trailing bytes.
- 88,342 accelerometer samples, 353,365 gyroscope samples, and 4,481 GPS9 samples.
- Stream timestamps (`STMP`) and cumulative sample counts (`TSMP`).
- GPS9 per-sample UTC times spanning 2026-10-03 13:20:58 through 13:28:26.
- Complex GPS9 data with `TYPE=lllllllSS`, and compressed records elsewhere.

These observations establish the reference for verification through the C
decoder. Keep this personal sample outside the repository.

`GPMF_Init()` in `GPMF_parser.c` already accepts binary memory and scans
consecutive `DEVC` records. No format extension is expected.
`demo/GPMF_demo.c` always opens an MP4 source, gates decoding on a positive
container duration, and requires `GetPayloadTime()` to succeed.
`OpenMP4Source()` requires an initial `ftyp` atom. The raw-related comment in
`OpenMP4SourceUDTA()` describes a size-prefixed `GPMF` atom, not this file format;
that path also invents a one-second duration.

At planning time, the GPX converter examined earlier was absent. Restore
or implement the exporter as part of this work, with both input routes using the
same GPS decoding and timestamp logic.

## Implementation steps

### 1. Add a bounded binary file reader

Add `demo/GPMF_rawreader.c` and `.h` with explicit open, read-next-record, and
close operations. Stream through the file rather than loading an arbitrarily
large file into one parser buffer. Return each complete top-level `DEVC` record
in allocated, at least 32-bit-aligned memory, retaining its file offset and index.

- Open in binary read-only mode and check all file and allocation operations.
- Require an eight-byte header, `DEVC` key, nesting type, and valid structure size.
- Calculate value length from structure size and big-endian repeat count;
  round stored length to four bytes and include the eight-byte header.
- Use wide arithmetic for file offsets and check sizes before allocation or
  conversion to the parser's `uint32_t` byte-count argument.
- Reject truncated headers, truncated values, invalid top-level records, and
  unexpected trailing data. Distinguish clean EOF from a failed read.
- Check nested record bounds and depth before recursive decoding, then run
  `GPMF_Validate()` and reset parser state before producing output. Confirm
  behavior for the sample's complex and compressed types.
- Release parser compression resources with `GPMF_Free()` before replacing a
  buffer; release the buffer and file handle on every exit path.

Do not equate a `DEVC` record with an original MP4 payload: a payload can contain
multiple devices, and concatenation may erase the grouping. Report raw record
indices and offsets without claiming to reconstruct the MP4 sample index.

### 2. Add explicit CLI routing and reuse decoding

Add `--raw` to the demo argument parser and help text. Keep existing invocations
on the MP4 path; explicit selection avoids guessing from a filename extension.
The intended command is:

```powershell
.\build\gpmf-parser.exe "C:\Video to Edit\Oct 3 Ride\Max2\GS010012.gpmf" --raw -g -a
```

Extract the existing record traversal and output into a shared processing helper
that accepts a buffer and optional timing context. Both source paths should call
that helper. For raw input:

- Process every record even when container duration is unavailable.
- Support structure output, FourCC filtering, scaled values, and all-record output.
- Label index output as raw record indices; expose byte offsets where useful.
- Return a nonzero process exit code for open, read, or decoding failures. The
  current `main()` returns zero regardless of `readMP4File()` failure; propagate
  failures while preserving successful invocation behavior.
- Keep MP4-specific fuzzing on the MP4 path; reject incompatible raw-mode options
  clearly instead of passing raw data to MP4 mutation logic.

### 3. Treat timing as optional metadata

Remove the positive-duration requirement from shared decoding and avoid an
unconditional `GetPayloadTime()` call for raw input. Do not manufacture duration,
video frame rate, edit-list offsets, or video synchronization information.

Interpret `STMP` as microseconds for the first stream sample and `TSMP` as a
cumulative sample count. Preserve these values as camera-relative metadata.
Report container timing as unavailable for raw input. Gate the existing
MP4-dependent sample-rate calculations; estimating rates from adjacent stream
timestamps can be a separate enhancement with explicit assumptions and matching
device/stream identities.

### 4. Export GPX using GPS-recorded UTC for both source formats

Route raw GPMF and MP4/MOV GPS data through the same exporter. The MP4 reader
provides the metadata bytes; its payload times must not affect GPX timestamps.

- For each GPS9 sample, apply `SCAL` and derive UTC directly from its
  days-since-2000 and seconds-since-midnight fields. Preserve the recorded
  fractional-second precision using integer arithmetic where practical.
- Decode GPS9 according to `TYPE=lllllllSS`: its final two fields are unsigned
  16-bit dilution-of-precision and fix-status values. Apply `--fix 3d|2d|all`:
  require 3D, require 2D or better (default), or ignore fix status. Missing fix
  metadata cannot satisfy a 2D/3D requirement. Validate coordinates and timestamp
  fields in every mode.
- Do not substitute `STMP`, MP4 presentation times, edit-list offsets, file
  dates, or video-derived sample intervals for GPS-recorded UTC. Correct the
  earlier converter's treatment of `STMP` as milliseconds if it is restored;
  retain camera-relative timing only for diagnostics.
- For older GPS5 data, use `GPSU` only for samples to which that recorded UTC
  timestamp can be reliably attributed. A block-level anchor is not an exact
  recorded timestamp for every sample. Do not synthesize intervening point times
  from video duration or sample-rate assumptions.
- When a valid position lacks an attributable GPS timestamp, export it without
  a GPX `<time>` element and report the number of untimed points. GPX timestamps
  that are present must always come from GPS-recorded time.
- Preserve recorded times and point order; do not alter timestamps to force
  monotonicity. Report unexpected regressions or duplicates where useful.

### 5. Update builds and documentation

Include the raw reader in the demo executable in `CMakeLists.txt` and
`demo/makefile`, and add the GPX exporter target to both build paths. Keep file
I/O in the demo layer; the installed parser library
does not need a new file API. Update `README.md` and CLI help with examples for
single-payload `.raw` files and concatenated `.gpmf` files, timing limitations,
and the distinction between raw records and original MP4 payloads. Document that
both GPX input modes use GPS-recorded UTC and how untimed points are represented.

### 6. Verify decoding, GPX timestamps, and compatibility

- Build using CMake and the demo Makefile on available supported toolchains.
- Decode the repository's `.raw` samples directly, including multiple-device
  examples, and compare decoded values with the corresponding MP4 metadata
  where payload correspondence can be established.
- Run the MAX2 reference file through the C decoder. Confirm full byte
  consumption, 448 top-level records, and the sensor sample counts listed above.
  Exercise GPS9 complex scaling and compressed data decoding.
- Check structure, scaled output, filtering, and all-record traversal. Verify
  that default first-record display does not masquerade as full-file output.
- Test empty input, short headers, short values, invalid keys, excessive nesting,
  unexpected trailing bytes, and records whose declared lengths exceed input.
  Confirm clear errors, nonzero exit status, and cleanup; use sanitizers when
  available.
- Run existing MP4 samples and timing/sample-rate options to verify that the
  shared-processing refactor preserves their output and timing behavior.
- Export identical GPS metadata through raw GPMF and MP4 input routes and compare
  point coordinates, fix filtering, and GPX timestamps. Both exports must match
  the GPS9 source date/time fields, including fractional seconds.
- Exercise an MP4 fixture whose container timestamps or edit-list offset differ
  while GPS metadata is unchanged. Its GPX point times must remain unchanged.
- Verify missing/invalid GPS timestamps, GPS5 block anchors, invalid fixes, UTC
  date rollover, and timestamp regressions. Confirm that missing GPS time never
  triggers a fallback to video or camera-relative time.

## Completion criteria

The documented `--raw` command consumes the supplied file without the original
video, traverses all records on request, and decodes sensor values through the
existing parser. Malformed input fails explicitly. Missing container timing does
not prevent decoding or produce fabricated timestamps. Existing MP4/MOV support
continues to work. GPX export supports both raw and MP4/MOV sources, uses recorded
GPS UTC for every timed point, and produces the same point times for equivalent
GPS metadata regardless of container timing.

## Implementation and verification (2026-10-06)

Implemented the bounded raw reader, shared source adapter, `--raw` demo route,
and `gpmf2gpx` exporter. Both input routes share GPS timestamp decoding. The core
parser's format API remains unchanged. The Windows MP4 reader now selects
64-bit stat/seek operations with native MinGW as well as Visual Studio, checks
payload reads, and retains allocations safely on failed buffer growth.

Native Windows builds use MSYS2 UCRT64 GCC 16.2.0 and GNU Make 4.4.1. Both the
CMake and demo Makefile targets build successfully. The Python integration suite
passes all nine tests when the personal reference is supplied; CTest runs the
portable fixtures and repository samples without requiring that file.

Reference verification confirms 448 records and 6,693,736 bytes, the sensor
sample counts above, GPS9 complex scaling, and compressed DISP decoding. GPX
export produces 3,559 timed points by default and excludes 922 samples without a
valid fix. `--fix 3d` produces 3,497 points; `--fix all` produces all 4,481 points.
Every exported coordinate and timestamp matches an independent binary decode.
Wrapping the exact reference metadata in an MP4 fixture, including altered
duration and edit-list offsets, produces identical GPX point data. Tests also
cover UTC rollover/leap days, missing/invalid times, unsigned fix fields,
duplicates/regressions, malformed input, and matching accelerometer values in
the repository's extracted files and original videos.

The nearby personal `GS010012.MP4` currently contains video, audio, and timecode
tracks, but no GPMF metadata track. It cannot supply the original metadata for
comparison; the reference MP4 fixture contains the extracted GPMF bytes instead.
The generated sample GPX is available at `build/native/max2-raw.gpx` locally.
