# Tests for the PT8 core and UI, built natively (Linux/macOS) without Windows.
#   make            build the test programs
#   make check      build and run them (with AddressSanitizer/UBSan)
#
# coretest can also compare playback against libxmp:
#   make XMP=/path/to/libxmp  (expects $(XMP)/include/xmp.h and $(XMP)/build/libxmp.a)

CC      ?= cc
CFLAGS  ?= -O1 -g
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
WARN    = -Wall -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation -Wno-unknown-warning-option
B       = build
TMP     ?= $(or $(TMPDIR),/tmp)/pt8-tests
DEMO    = ../examples/demo8.mod

CORE    = ../src/song.c ../src/replayer.c
APP     = ../src/app.c $(CORE)
HDRS    = $(wildcard ../src/*.h) testenv.h

ifdef XMP
XMPFLAGS = -DWITH_XMP -I$(XMP)/include
XMPLIB   = $(XMP)/build/libxmp.a
endif

all: $(B)/coretest $(B)/functest $(B)/monkey $(B)/uiharness

$(B):
	mkdir -p $(B)

$(B)/coretest: coretest.c $(CORE) $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(WARN) $(XMPFLAGS) -I../src coretest.c $(CORE) $(XMPLIB) -lm -o $@

$(B)/functest: functest.c $(APP) $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I../src functest.c $(APP) -lm -o $@

$(B)/monkey: monkey.c $(APP) $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(SAN) $(WARN) -I../src monkey.c $(APP) -lm -o $@

$(B)/uiharness: uiharness.c $(APP) $(HDRS) | $(B)
	$(CC) $(CFLAGS) $(WARN) -I../src uiharness.c $(APP) -lm -o $@

check: all
	mkdir -p $(TMP)
	./$(B)/functest
	./$(B)/monkey $(DEMO) 6000 1
	./$(B)/coretest roundtrip $(DEMO) $(TMP)/roundtrip.mod
	./$(B)/coretest render $(DEMO) $(TMP)/demo8.wav
	./$(B)/uiharness $(DEMO) > /dev/null
	@echo "all tests passed (output files in $(TMP))"

clean:
	rm -rf $(B)

.PHONY: all check clean
