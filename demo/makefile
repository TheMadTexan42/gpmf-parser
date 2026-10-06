# Address-sanitizer library
CC ?= gcc
CFLAGS ?= -g
LDLIBS := -lm
ASAN_FLAGS :=
ifneq ($(OS),Windows_NT)
	UNAME_S := $(shell uname -s)
	ASAN_FLAGS := -fsanitize=address
	ifeq ($(UNAME_S),Darwin)
		# macOS requires different a flag
		ASAN_FLAGS := -fsanitize=address
	endif
endif

.PHONY: all clean
all: gpmfdemo gpmf2gpx

READERS := GPMF_mp4reader.o GPMF_rawreader.o GPMF_source.o
gpmfdemo : GPMF_demo.o GPMF_parser.o GPMF_utils.o $(READERS) GPMF_print.o
		$(CC) -o $@ $^ $(ASAN_FLAGS) $(LDLIBS)

gpmf2gpx : GPMF_to_gpx.o GPMF_parser.o $(READERS)
		$(CC) -o $@ $^ $(ASAN_FLAGS) $(LDLIBS)

GPMF_rawreader.o: GPMF_rawreader.c GPMF_rawreader.h ../GPMF_parser.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c $<
GPMF_source.o: GPMF_source.c GPMF_source.h GPMF_rawreader.h GPMF_mp4reader.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c $<
GPMF_to_gpx.o: GPMF_to_gpx.c GPMF_source.h ../GPMF_parser.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c $<

GPMF_demo.o : GPMF_demo.c GPMF_source.h GPMF_rawreader.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c GPMF_demo.c
GPMF_mp4reader.o : GPMF_mp4reader.c ../GPMF_parser.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c GPMF_mp4reader.c
GPMF_print.o : GPMF_print.c ../GPMF_parser.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c GPMF_print.c
GPMF_parser.o : ../GPMF_parser.c ../GPMF_parser.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c ../GPMF_parser.c
GPMF_utils.o : ../GPMF_utils.c ../GPMF_utils.h
		$(CC) $(CFLAGS) $(ASAN_FLAGS) -c ../GPMF_utils.c
clean :
		rm -f gpmfdemo gpmf2gpx *.o
