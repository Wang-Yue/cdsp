# Detect layout:
# If Sources/CDSP directory exists in current working dir, we are in root of parent project (.)
# Else if ../../Sources/CDSP exists, we are in Sources/CDSP subdirectory of parent project
# Otherwise, we are in the standalone CDSP repository root
ifeq ($(shell [ -d Sources/CDSP ] && echo parent_root || ( [ -d ../../Sources/CDSP ] && echo parent_sub || echo standalone )),parent_root)
    ROOT_DIR := .
    SRC_ROOT := ./Sources/CDSP
else ifeq ($(shell [ -d ../../Sources/CDSP ] && echo parent_sub || echo standalone),parent_sub)
    ROOT_DIR := ../..
    SRC_ROOT := ../../Sources/CDSP
else
    ROOT_DIR := .
    SRC_ROOT := .
endif

CC ?= clang
AR ?= ar
CFLAGS ?= -O3 -flto -ffp-contract=fast -fno-math-errno -funroll-loops -fvisibility=hidden -DCDSP_BUILD_SHARED -Wall -Wextra -std=c11 -I$(ROOT_DIR) -I$(SRC_ROOT) -I$(SRC_ROOT)/Filters -I$(SRC_ROOT)/Audio -I$(SRC_ROOT)/Config -I$(SRC_ROOT)/FFT -I$(SRC_ROOT)/Mixer -I$(SRC_ROOT)/Resampler -I$(SRC_ROOT)/Processors -I$(SRC_ROOT)/DoP -I$(SRC_ROOT)/Pipeline -I$(SRC_ROOT)/Engine -I$(SRC_ROOT)/Server -I$(SRC_ROOT)/Backend -I$(SRC_ROOT)/Logging -I$(SRC_ROOT)/Utils
UNAME_S := $(shell uname -s)

ifeq ($(IS_WINDOWS),1)
    IS_DARWIN := 0
    IS_LINUX := 0
else ifneq (,$(filter Windows_NT MINGW% MSYS% CYGWIN%,$(UNAME_S)))
    IS_WINDOWS := 1
    IS_DARWIN := 0
    IS_LINUX := 0
else ifeq ($(UNAME_S),Darwin)
    IS_DARWIN := 1
    IS_WINDOWS := 0
    IS_LINUX := 0
else
    IS_LINUX := 1
    IS_WINDOWS := 0
    IS_DARWIN := 0
endif

ifeq ($(IS_WINDOWS),1)
    CC := clang
    AR := llvm-ar
    ENABLE_COREAUDIO ?= 0
    ENABLE_ACCELERATE ?= 0
    ENABLE_ALSA ?= 0
    ENABLE_PIPEWIRE ?= 0
    ENABLE_FFTW ?= 1
    ENABLE_BLAS ?= 0
    ENABLE_WASAPI ?= 1
    ENABLE_ASIO ?= 1
    ENABLE_OPENMP ?= 0
    CLI_BIN_EXT := .exe
else ifeq ($(IS_DARWIN),1)
    CFLAGS += -mcpu=native
    LDFLAGS += -flto
    ENABLE_COREAUDIO ?= 1
    ENABLE_ACCELERATE ?= 1
    ENABLE_ALSA ?= 0
    ENABLE_PIPEWIRE ?= 0
    ENABLE_FFTW ?= 0
    ENABLE_BLAS ?= 0
    ENABLE_WASAPI ?= 0
    ENABLE_ASIO ?= 0
    ENABLE_OPENMP ?= 0
else
    # Linux (Default)
    ifeq ($(CROSS_COMPILE),)
        CFLAGS += -march=native
    endif
    ENABLE_COREAUDIO ?= 0
    ENABLE_ACCELERATE ?= 0
    ENABLE_ALSA ?= 1
    ENABLE_PIPEWIRE ?= 1
    ENABLE_FFTW ?= 1
    ENABLE_BLAS ?= 0
    ENABLE_WASAPI ?= 0
    ENABLE_ASIO ?= 0
    ENABLE_OPENMP ?= 0
endif

# Map Flags to CFLAGS preprocessor definitions
$(foreach f,COREAUDIO ACCELERATE ALSA PIPEWIRE FFTW BLAS WASAPI ASIO,$(if $(filter 1,$(ENABLE_$(f))),$(eval CFLAGS += -DENABLE_$(f))))


# Map Flags to link options (LDFLAGS)
LDFLAGS += -lm -lpthread
ifeq ($(IS_WINDOWS),1)
    # Windows Setup
    CFLAGS += -DCOBJMACROS -D_WIN32 -DUNICODE -D_UNICODE -D_USE_MATH_DEFINES -I$(ROOT_DIR)/win_deps/include
    ifeq ($(ENABLE_FFTW),1)
        LDFLAGS += -L$(ROOT_DIR)/win_deps/lib -lfftw3 -lfftw3f
    else
        LDFLAGS += -L$(ROOT_DIR)/win_deps/lib
    endif
    ifeq ($(ENABLE_BLAS),1)
        LDFLAGS += -lopenblas
    endif
    LDFLAGS += -lws2_32 -lwinmm -lole32 -luuid -lksuser -lksguid
else ifeq ($(IS_DARWIN),1)
    ifeq ($(ENABLE_COREAUDIO),1)
        LDFLAGS += -framework CoreAudio -framework AudioToolbox
    endif
    ifeq ($(ENABLE_ACCELERATE),1)
        LDFLAGS += -framework Accelerate
    endif
    ifeq ($(ENABLE_FFTW),1)
        CFLAGS += -I/opt/homebrew/include
        LDFLAGS += -L/opt/homebrew/lib -lfftw3 -lfftw3f
    endif
    LDFLAGS += -framework CoreFoundation
else
    # Linux
    CFLAGS += -D_GNU_SOURCE
    ifneq ($(ENABLE_DBUS),0)
        CFLAGS += $(shell pkg-config --cflags dbus-1 2>/dev/null || echo "")
        LDFLAGS += $(shell pkg-config --libs dbus-1 2>/dev/null || echo "-ldbus-1")
    else
        CFLAGS += -DNO_DBUS
    endif
    ifeq ($(USE_LIBDISPATCH),1)
        CFLAGS += -DUSE_LIBDISPATCH $(shell pkg-config --cflags libdispatch 2>/dev/null || echo "-I/usr/libexec/swift/lib/swift")
        LDFLAGS += $(shell pkg-config --libs libdispatch 2>/dev/null || echo "-L/usr/libexec/swift/lib/swift/linux -ldispatch -lBlocksRuntime -Wl,-rpath,/usr/libexec/swift/lib/swift/linux")
    else
        ifeq ($(ENABLE_OPENMP),1)
            CFLAGS += -fopenmp -DUSE_OPENMP
        endif
    endif
    ifeq ($(ENABLE_ALSA),1)
        LDFLAGS += -lasound
    endif
    ifeq ($(ENABLE_PIPEWIRE),1)
        LDFLAGS += -lpipewire-0.3
        CFLAGS += $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null || echo "")
    endif
    ifeq ($(ENABLE_FFTW),1)
        LDFLAGS += -lfftw3 -lfftw3f
    endif
    ifeq ($(ENABLE_BLAS),1)
        LDFLAGS += -lopenblas
    endif
    LDFLAGS += -lrt
    ifneq ($(USE_LIBDISPATCH),1)
        ifeq ($(ENABLE_OPENMP),1)
            LDFLAGS += -fopenmp
        endif
    endif
endif


# All library C source files across subdirectories of CDSP
ALL_SRCS := $(wildcard $(SRC_ROOT)/*/*.c)

# Filter native source files based on enabled backends
SRCS := $(ALL_SRCS)

ifneq ($(ENABLE_ALSA),1)
    SRCS := $(filter-out %/alsa_capture.c %/alsa_playback.c %/alsa_device.c %/alsa_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_PIPEWIRE),1)
    SRCS := $(filter-out %/pipewire_backend.c, $(SRCS))
endif

ifneq ($(ENABLE_COREAUDIO),1)
    SRCS := $(filter-out %/core_audio_capture.c %/core_audio_playback.c %/core_audio_device.c %/core_audio_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_WASAPI),1)
    SRCS := $(filter-out %/wasapi_capture.c %/wasapi_playback.c %/wasapi_device.c %/wasapi_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_ASIO),1)
    SRCS := $(filter-out %/asio_backend.c %/asio_capabilities.c, $(SRCS))
endif



# Exclude Server directory files from libdsp.a
SRCS := $(filter-out $(SRC_ROOT)/Server/%.c %/Server/%.c, $(SRCS))

# Map source files to object files in .build/obj/
# e.g. ./Sources/CDSP/Audio/audio_buffers.c -> ./build/obj/Sources/CDSP/Audio/audio_buffers.o
OBJ_DIR := $(ROOT_DIR)/.build/obj
OBJS := $(patsubst $(ROOT_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TEST_OBJ_DIR := $(ROOT_DIR)/.build/obj_test
TEST_OBJS := $(patsubst $(ROOT_DIR)/%.c, $(TEST_OBJ_DIR)/%.o, $(SRCS))
TEST_LIB_TARGET := $(SRC_ROOT)/libdsp_test.a

LIB_TARGET := $(SRC_ROOT)/libdsp.a
SERVER_SRCS := $(wildcard $(SRC_ROOT)/Server/*.c)
CLI_SRC := $(SRC_ROOT)/main.c
CLI_BIN := $(SRC_ROOT)/bin/dsp-cli$(CLI_BIN_EXT)

TEST_SRCS := $(wildcard $(ROOT_DIR)/Tests/CLibTests/test_*.c)
TEST_BINS := $(patsubst $(ROOT_DIR)/Tests/CLibTests/test_%.c, $(ROOT_DIR)/Tests/CLibTests/bin/test_%, $(TEST_SRCS))

.PHONY: all build lib test run-test-runner bench cli clean format

all: cli

build: all

lib: $(LIB_TARGET)

# Compile library source files into object files (once per file)
$(OBJ_DIR)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile library source files with CDSP_TEST define for tests
$(TEST_OBJ_DIR)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DCDSP_TEST -c $< -o $@

# Archive object files into static library
$(LIB_TARGET): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

# Archive test object files into static library
$(TEST_LIB_TARGET): $(TEST_OBJS)
	@rm -f $@
	$(AR) rcs $@ $(TEST_OBJS)

BENCH_NAMES := test_filter_benchmark test_dop_benchmark test_pipeline_benchmark test_resampler_matrix
BENCH_BINS := $(patsubst %, $(ROOT_DIR)/Tests/CLibTests/bin/%, $(BENCH_NAMES))

UNIT_TEST_SRCS := $(filter-out %/test_runner_main.c %/test_websocket_server.c %/test_filter_benchmark.c %/test_dop_benchmark.c %/test_pipeline_benchmark.c %/test_resampler_matrix.c, $(wildcard $(ROOT_DIR)/Tests/CLibTests/test_*.c))
UNIT_TEST_RUNNER := $(ROOT_DIR)/Tests/CLibTests/bin/test_runner
UNIT_TEST_BINS := $(UNIT_TEST_RUNNER) $(ROOT_DIR)/Tests/CLibTests/bin/test_websocket_server

# Build benchmark binaries linked against main library (without clock_mock)
$(BENCH_BINS): $(ROOT_DIR)/Tests/CLibTests/bin/%: $(ROOT_DIR)/Tests/CLibTests/%.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

# Build combined unit test runner binary
$(UNIT_TEST_RUNNER): $(UNIT_TEST_SRCS) $(ROOT_DIR)/Tests/CLibTests/test_runner_main.c $(TEST_LIB_TARGET)
	@mkdir -p $(dir $@)
ifeq ($(IS_WINDOWS),1)
	$(CC) $(CFLAGS) -DCDSP_TEST -DCDSP_COMBINED_TEST_SUITE $^ $(LDFLAGS) -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc -Wl,--wrap,free -Wl,--wrap,CoCreateInstance -Wl,--wrap,RegOpenKeyExA -Wl,--wrap,RegEnumKeyA -Wl,--wrap,RegQueryValueExA -Wl,--wrap,RegCloseKey -o $@
else
	$(CC) $(CFLAGS) -DCDSP_TEST -DCDSP_COMBINED_TEST_SUITE $^ $(LDFLAGS) -o $@
endif

$(ROOT_DIR)/Tests/CLibTests/bin/test_websocket_server: $(ROOT_DIR)/Tests/CLibTests/test_websocket_server.c $(SERVER_SRCS) $(TEST_LIB_TARGET)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DCDSP_TEST $^ $(LDFLAGS) -o $@


RUST_HARNESS_DIR := $(ROOT_DIR)/Tests/RustHarnesses

test-rust-build:
	@if [ -d "$(RUST_HARNESS_DIR)" ]; then \
		echo "🦀 Building Rust harness binaries..."; \
		cd $(RUST_HARNESS_DIR) && cargo build --release; \
	fi

ALL_TEST_CASES :=

define DEFINE_CASE_RULE
.PHONY: case_$(1)_$(2)
case_$(1)_$(2):
	@$(3) --run $(2) > /dev/null 2>&1 && echo "✅ $(1):$(2) passed" || (echo "❌ $(1):$(2) failed" && exit 1)
ALL_TEST_CASES += case_$(1)_$(2)
endef

define DEFINE_BIN_RULE
.PHONY: case_$(1)_all
case_$(1)_all:
	@$(2) > /dev/null 2>&1 && echo "✅ $(1) passed" || (echo "❌ $(1) failed" && exit 1)
ALL_TEST_CASES += case_$(1)_all
endef

$(foreach bin,$(UNIT_TEST_BINS),\
  $(eval CASES := $(shell $(bin) --list 2>/dev/null))\
  $(if $(CASES),\
    $(foreach c,$(CASES),\
      $(eval $(call DEFINE_CASE_RULE,$(notdir $(bin)),$(c),$(bin)))\
    ),\
    $(eval $(call DEFINE_BIN_RULE,$(notdir $(bin)),$(bin)))\
  )\
)

.PHONY: callgraph-audit
callgraph-audit:
	@python3 Tools/generate_callgraph.py > /dev/null && echo "✅ Static Call Graph Audit passed" || (echo "❌ Static Call Graph Audit failed" && exit 1)

test: test-rust-build $(UNIT_TEST_BINS)
	+@$(MAKE) run-test-runner

SAN_BASE_CFLAGS := $(filter-out -O3 -flto -ffp-contract=fast -fno-math-errno -funroll-loops -fvisibility=hidden -mcpu=native -DCDSP_BUILD_SHARED,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer

# NOTE: On macOS (Apple Silicon ARM64), Apple Clang (/usr/bin/clang) has a known dynamic runtime
# initializer deadlock/crash bug in libclang_rt.asan_osx_dynamic.dylib and libclang_rt.tsan_osx_dynamic.dylib.
# Do NOT use Xcode Apple Clang for sanitizer runs. Use Homebrew LLVM Clang instead:
#   CC=/opt/homebrew/opt/llvm/bin/clang make test-asan
#   CC=/opt/homebrew/opt/llvm/bin/clang make test-tsan

.PHONY: test-asan test-tsan
test-asan:
	@echo "\n🩺 Running Unit Tests under AddressSanitizer & UndefinedBehaviorSanitizer...\n"
	$(MAKE) clean
	+$(MAKE) -j test CFLAGS="$(SAN_BASE_CFLAGS) -fsanitize=address,undefined" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

test-tsan:
	@echo "\n🩺 Running Unit Tests under ThreadSanitizer...\n"
	$(MAKE) clean
	+$(MAKE) -j test CFLAGS="$(SAN_BASE_CFLAGS) -fsanitize=thread" LDFLAGS="$(LDFLAGS) -fsanitize=thread"

run-test-runner:
	@echo "\n🚀 Running $(words $(ALL_TEST_CASES)) test cases in parallel using Makefile Jobserver...\n"
	+@$(MAKE) $(ALL_TEST_CASES)


bench: test-rust-build $(BENCH_BINS)
	@echo "\n=== Running C Benchmark Tests ==="
	@for bin in $(BENCH_BINS); do \
		echo ""; \
		$$bin || exit 1; \
	done
	@echo "\n✅ All C benchmarks passed!"

cli: $(CLI_BIN)

$(CLI_BIN): $(CLI_SRC) $(SERVER_SRCS) $(LIB_TARGET)
	@mkdir -p $(dir $@)
ifeq ($(IS_WINDOWS),1)
	$(CC) $(CFLAGS) $(CLI_SRC) $(SERVER_SRCS) -Wl,--whole-archive $(LIB_TARGET) -Wl,--no-whole-archive $(LDFLAGS) -o $@
else ifeq ($(IS_DARWIN),1)
	$(CC) $(CFLAGS) $(CLI_SRC) $(SERVER_SRCS) -Wl,-force_load $(LIB_TARGET) $(LDFLAGS) -o $@
else
	$(CC) $(CFLAGS) $(CLI_SRC) $(SERVER_SRCS) -Wl,--whole-archive $(LIB_TARGET) -Wl,--no-whole-archive $(LDFLAGS) -o $@
endif

format:
	find $(ROOT_DIR) \( -name "*.c" -o -name "*.h" \) -not -path "*/.build/*" -not -path "*/Tests/RustHarnesses/*" | xargs clang-format -i

clean:
	rm -rf $(OBJ_DIR) $(TEST_OBJ_DIR) $(LIB_TARGET) $(TEST_LIB_TARGET) $(ROOT_DIR)/Tests/CLibTests/bin $(SRC_ROOT)/bin
