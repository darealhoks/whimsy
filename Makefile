MODE ?= dev
CC ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Icore -Ivendor/monocypher -D_DEFAULT_SOURCE
ifeq ($(MODE),dev)
CFLAGS += -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif

MONO := vendor/monocypher/monocypher.c
CORE := $(wildcard core/*.c) $(MONO)
GUI := $(wildcard gui/*.c)
BIN := build/$(MODE)

# the gui is built only where sdl3 and freetype2 are installed
GUI_PKGS := sdl3 freetype2
HAVE_GUI := $(shell pkg-config --exists $(GUI_PKGS) && echo 1)
GUI_CFLAGS := $(shell pkg-config --cflags $(GUI_PKGS) 2>/dev/null)
GUI_LIBS := $(shell pkg-config --libs $(GUI_PKGS) 2>/dev/null)
GUI_INC := -Igui -Ivendor/stb
GUI_HDRS := $(notdir $(wildcard gui/*.h) $(wildcard gui/*.c))

all: $(BIN)/libwhimsy.a $(if $(wildcard server/*.c),$(BIN)/whimsyd) $(if $(HAVE_GUI),$(BIN)/whimsy)

$(BIN)/whimsy: $(GUI) $(BIN)/libwhimsy.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(GUI_INC) $(GUI_CFLAGS) $(LDFLAGS) -o $@ $(GUI) $(BIN)/libwhimsy.a $(GUI_LIBS) -lm

$(BIN)/libwhimsy.a: $(CORE:%.c=$(BIN)/%.o)
	@mkdir -p $(@D)
	ar rcs $@ $^

$(BIN)/whimsyd: $(wildcard server/*.c) $(BIN)/libwhimsy.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

$(BIN)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

test: all $(wildcard tests/*.c)
	@for f in core/*.[ch] server/*.c gui/*.[ch]; do \
	  n=$$(wc -l < $$f); [ $$n -gt 1000 ] && echo "$$f: $$n lines, over 1000" && bad=1; done; \
	  [ -z "$$bad" ] || exit 1
	@for t in tests/test_*.c; do \
	  b=$$(basename $$t .c); g=""; \
	  for h in $(GUI_HDRS); do grep -q "\"$$h\"" $$t && g="$(GUI_INC) $(GUI_CFLAGS) $(GUI_LIBS)"; done; \
	  if [ -n "$$g" ] && [ -z "$(HAVE_GUI)" ]; then echo "skip $$b: no sdl3"; continue; fi; \
	  $(CC) $(CFLAGS) $$g $(LDFLAGS) -o $(BIN)/$$b $$t $(BIN)/libwhimsy.a -lm || exit 1; \
	  $(BIN)/$$b $(BIN) || exit 1; done

# fuzz targets live in tests/fuzz_*.c, one LLVMFuzzerTestOneInput each
fuzz: $(CORE)
	@for f in tests/fuzz_*.c; do \
	  clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -Icore -Igui -Ivendor/monocypher -D_DEFAULT_SOURCE -o build/$$(basename $$f .c) $$f $(CORE) || exit 1; done

clean:
	rm -rf build

.PHONY: all test fuzz clean
