# C23/C++23 Safety Reference

Comprehensive guide to catching bugs at compile-time, runtime, and with tooling.

---

## Table of Contents

1. [Compile-Time Safety](#compile-time-safety)
2. [Runtime Crash Handling](#runtime-crash-handling)
3. [Memory Safety Tools](#memory-safety-tools)
4. [Static Analysis](#static-analysis)
5. [Quick Reference](#quick-reference)
6. [Checklist](#checklist)

---

# Compile-Time Safety

Catch bugs before the program runs.

---

## Attributes

### `[[nodiscard]]` / `[[nodiscard("reason")]]`
Warn if return value is ignored. Use on error codes, allocated memory, computed results.

```c
[[nodiscard]] int parse_config(const char *path);
[[nodiscard("memory must be freed")]] void *allocate_buffer(size_t n);

parse_config("foo.ini");  // WARNING: ignoring return value
```

### `[[maybe_unused]]`
Suppress "unused variable" warnings intentionally. Useful for conditional compilation.

```c
[[maybe_unused]] static void debug_helper(void) { ... }

void foo([[maybe_unused]] int reserved_param) {
    // param unused now, but API requires it
}
```

### `[[deprecated]]` / `[[deprecated("reason")]]`
Mark old APIs. Compiler warns on use.

```c
[[deprecated("use new_api() instead")]]
void old_api(void);
```

### `[[noreturn]]`
Function never returns (calls exit, abort, longjmp, infinite loop).

```c
[[noreturn]] void fatal_error(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}
```

### `[[fallthrough]]`
Intentional switch fallthrough (suppresses warning).

```c
switch (x) {
case 1:
    do_thing();
    [[fallthrough]];
case 2:
    do_other_thing();
    break;
}
```

### `[[assume(expr)]]` (C++23)
Optimization hint. Undefined behavior if expr is false.

```cpp
[[assume(x > 0)]];
// Compiler can optimize assuming x > 0
```

---

## Compile-Time Assertions

### `static_assert(expr, "message")`
Fail compilation if expression is false. No runtime cost.

```c
// Validate struct sizes match kernel ABI
static_assert(sizeof(struct drm_mode_crtc_lut) == 32,
              "struct size mismatch with kernel");

// Validate array sizes
static_assert(ARRAY_SIZE == 256, "array must have 256 entries");

// Validate type sizes
static_assert(sizeof(int) == 4, "int must be 32-bit");

// Validate enum values
static_assert(ERROR_MAX < 256, "error codes must fit in uint8_t");
```

**Use cases:**
- Kernel/hardware struct size validation
- Buffer size bounds
- Enum range validation
- Platform assumptions (int size, pointer size)

---

## Type Safety

### `nullptr` (C23)
Type-safe null pointer constant. Replaces `NULL`.

```c
int *p = nullptr;           // OK
int x = nullptr;            // ERROR: nullptr is not an integer
void *v = nullptr;          // OK
if (p == nullptr) { ... }   // OK
```

### `constexpr` (C23/C++11+)
Compile-time evaluated constant. Stronger than `const`.

```c
constexpr int TABLE_SIZE = 256;
constexpr float PI = 3.14159265f;

// Arrays can use constexpr size
int buffer[TABLE_SIZE];

// Lookup tables evaluated at compile-time
static constexpr float lut[] = { 1.0f, 2.0f, 3.0f };
```

### `consteval` (C++20+)
MUST be evaluated at compile-time (stricter than constexpr).

```cpp
consteval int square(int n) { return n * n; }
int a = square(5);   // OK, computed at compile-time
int x = 5;
int b = square(x);   // ERROR: x not constexpr
```

### `constinit` (C++20+)
Must be constant-initialized (but can be modified later).

```cpp
constinit int global = compute_initial();  // Must be constant-initialized
// global can be modified at runtime
```

### `typeof` / `typeof_unqual` (C23)
Type inference from expressions.

```c
int x = 5;
typeof(x) y = 10;           // y is int
typeof_unqual(x) z = 15;    // z is int (strips const/volatile)

// Useful in macros
#define MAX(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a > _b ? _a : _b; })
```

---

## Pointer Qualifiers

### `const` Correctness
Mark data as read-only. Compiler enforces.

```c
// Function won't modify the string
size_t strlen(const char *s);

// Function won't modify the state
int get_count(const state_t *state);

// Pointer to const data
const int *p;       // *p is read-only, p can change
int *const p;       // p is read-only, *p can change
const int *const p; // both read-only
```

### `restrict` (C99+)
Promise that pointers don't alias. Enables optimization.

```c
// Compiler can assume dst and src don't overlap
void copy(int *restrict dst, const int *restrict src, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
}
```

### `volatile`
Prevent compiler optimization. For hardware registers, signal handlers.

```c
volatile int *hardware_reg = (volatile int *)0xFFFF0000;
*hardware_reg = 1;  // Compiler MUST write, even if value seems unused
```

---

## Compiler Flags (GCC/Clang)

### Essential
```makefile
CFLAGS += -Wall -Wextra -Wpedantic
```
- `-Wall` — common warnings
- `-Wextra` — extra warnings
- `-Wpedantic` — strict ISO C compliance

### Recommended
```makefile
CFLAGS += -Wshadow -Wconversion -Wcast-qual -Wformat=2
```
- `-Wshadow` — variable shadows outer scope
- `-Wconversion` — implicit type conversions
- `-Wcast-qual` — casting away const/volatile
- `-Wformat=2` — printf/scanf format string checks

### For CI/Release
```makefile
CFLAGS += -Werror
```
- `-Werror` — treat warnings as errors

### Full Example
```makefile
CFLAGS := -std=c2x -O2 -march=native -fPIC
CFLAGS += -Wall -Wextra -Wpedantic
CFLAGS += -Wshadow -Wconversion -Wcast-qual -Wformat=2
# CFLAGS += -Werror  # Enable for CI
```

---

# Runtime Crash Handling

When prevention fails, get actionable crash information.

---

## Signal-Based Crash Handler (Linux/POSIX)

Catch fatal signals and print stack traces before dying.

### Signals to Handle

| Signal | Cause |
|--------|-------|
| `SIGSEGV` | Segmentation fault (null deref, bad pointer) |
| `SIGABRT` | Abort (assert failure, `std::terminate`) |
| `SIGFPE` | Floating point exception (div by zero) |
| `SIGILL` | Illegal instruction |
| `SIGBUS` | Bus error (alignment, bad memory access) |

### Minimal Handler (No Dependencies)

```cpp
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>  // Linux/glibc

[[noreturn]] void crash_handler(int sig) {
    constexpr int MAX_FRAMES = 64;
    void *frames[MAX_FRAMES];

    int n = backtrace(frames, MAX_FRAMES);

    fprintf(stderr, "\n=== CRASH: Signal %d ===\n", sig);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    _exit(1);
}

void install_crash_handler() {
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGFPE,  crash_handler);
    std::signal(SIGILL,  crash_handler);
    std::signal(SIGBUS,  crash_handler);
}
```

Compile with `-rdynamic` for symbol names.

### Full Handler with Source Context

Use **backward-cpp** for rich stack traces with source code snippets.
See `crash_handler.hpp` in this directory.

**Requirements:**
```bash
# Ubuntu/Debian
sudo apt install libdw-dev

# Compile flags
CXXFLAGS += -g -DBACKWARD_HAS_DW=1
LDFLAGS  += -ldw
```

**Usage:**
```cpp
#include "crash_handler.hpp"

int main() {
    CrashHandler::Install();
    // ... your code ...
}
```

### `sigaction` vs `signal` (Preferred)

`sigaction` gives more control and is more portable:

```cpp
#include <signal.h>

void install_crash_handler() {
    struct sigaction sa{};
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_RESETHAND;  // One-shot, prevent recursion
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
}
```

### Async-Signal-Safety

Only use async-signal-safe functions in handlers:
- `write()`, `_exit()`, `signal()`
- **NOT safe:** `printf()`, `malloc()`, `new`, most of libc

`dprintf(fd, ...)` is safe. `fprintf(stderr, ...)` is not.

---

## Structured Exception Handling (Windows)

Windows equivalent using SEH and DbgHelp.

### Minimal Handler

```cpp
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

LONG WINAPI crash_handler(EXCEPTION_POINTERS *ex) {
    fprintf(stderr, "\n=== CRASH: Exception 0x%08X ===\n",
            ex->ExceptionRecord->ExceptionCode);

    // Walk stack
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    CONTEXT *ctx = ex->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    while (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process,
                       GetCurrentThread(), &frame, ctx,
                       NULL, SymFunctionTableAccess64,
                       SymGetModuleBase64, NULL)) {
        char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;

        if (SymFromAddr(process, frame.AddrPC.Offset, NULL, symbol)) {
            fprintf(stderr, "  %s\n", symbol->Name);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

void install_crash_handler() {
    SetUnhandledExceptionFilter(crash_handler);
}
```

### Windows Exception Codes

| Code | Meaning |
|------|---------|
| `0xC0000005` | Access violation (SIGSEGV equivalent) |
| `0xC0000094` | Integer divide by zero |
| `0xC00000FD` | Stack overflow |
| `0xC0000409` | Stack buffer overrun |

---

## Stack Trace Libraries

| Library | Platform | Features |
|---------|----------|----------|
| [backward-cpp](https://github.com/bombela/backward-cpp) | Linux, macOS | Source snippets, header-only |
| [cpptrace](https://github.com/jeremy-rifkin/cpptrace) | Cross-platform | Modern C++, easy setup |
| [StackWalker](https://github.com/JochenKalmbach/StackWalker) | Windows | Single header |
| [libunwind](https://www.nongnu.org/libunwind/) | Linux, macOS | Low-level, fast |
| [Boost.Stacktrace](https://www.boost.org/doc/libs/release/doc/html/stacktrace.html) | Cross-platform | Part of Boost |

---

# Memory Safety Tools

Detect leaks, buffer overflows, and undefined behavior.

---

## AddressSanitizer (ASan)

Detects buffer overflows, use-after-free, use-after-return, memory leaks.

### Usage

```makefile
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=address
```

```bash
./my_program  # Crashes with detailed report on error
```

### Example Output

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000014
READ of size 4 at 0x602000000014 thread T0
    #0 0x4011a3 in main test.c:8
    #1 0x7f123... in __libc_start_main

0x602000000014 is located 0 bytes after 20-byte region [0x602000000000,0x602000000014)
allocated by thread T0 here:
    #0 0x7f123... in malloc
    #1 0x401153 in main test.c:6
```

### Options (Environment Variables)

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./my_program
```

| Option | Effect |
|--------|--------|
| `detect_leaks=1` | Enable leak detection (default on Linux) |
| `halt_on_error=0` | Continue after first error |
| `symbolize=1` | Show source locations |
| `abort_on_error=1` | Call abort() on error (for core dumps) |

---

## LeakSanitizer (LSan)

Dedicated memory leak detector. Included with ASan, or standalone.

### Standalone Usage

```makefile
CFLAGS  += -fsanitize=leak -g
LDFLAGS += -fsanitize=leak
```

### Suppressing False Positives

Create `lsan.supp`:
```
leak:third_party_lib
leak:legacy_code.c
```

```bash
LSAN_OPTIONS=suppressions=lsan.supp ./my_program
```

---

## UndefinedBehaviorSanitizer (UBSan)

Detects undefined behavior: signed overflow, null deref, misaligned access, etc.

### Usage

```makefile
CFLAGS  += -fsanitize=undefined -g
LDFLAGS += -fsanitize=undefined
```

### What It Catches

- Signed integer overflow
- Null pointer dereference
- Misaligned pointer access
- Out-of-bounds array indexing
- Division by zero
- Invalid bool/enum values
- Pointer overflow

### Combining Sanitizers

```makefile
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=address,undefined
```

**Note:** ASan and TSan (ThreadSanitizer) cannot be combined.

---

## ThreadSanitizer (TSan)

Detects data races in multithreaded programs.

### Usage

```makefile
CFLAGS  += -fsanitize=thread -g
LDFLAGS += -fsanitize=thread
```

### Example Output

```
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 4 at 0x7f... by thread T1:
    #0 increment() race.c:10
  Previous read of size 4 at 0x7f... by main thread:
    #0 main() race.c:20
```

---

## Valgrind (Linux)

Heavyweight memory checker. No recompilation needed, but 10-50x slower.

### Memcheck (Memory Errors)

```bash
valgrind --leak-check=full --show-leak-kinds=all ./my_program
```

### Common Errors Detected

| Error | Meaning |
|-------|---------|
| Invalid read/write | Buffer overflow |
| Use of uninitialised value | Uninitialized memory |
| Invalid free | Double free, bad pointer |
| Definitely lost | Memory leak |
| Indirectly lost | Leaked via another leak |

### Suppressions

Generate suppressions for false positives:
```bash
valgrind --gen-suppressions=yes ./my_program
```

Save to `valgrind.supp` and use:
```bash
valgrind --suppressions=valgrind.supp ./my_program
```

---

## Dr. Memory (Windows/Linux)

Valgrind alternative, works on Windows.

### Usage

```bash
drmemory -- ./my_program.exe
```

### What It Catches

- Uninitialized reads
- Buffer overflows (heap/stack)
- Use-after-free
- Memory leaks
- Handle leaks (Windows)
- GDI object leaks (Windows)

---

## Sanitizer Comparison

| Tool | Speed | Recompile? | Platform | Best For |
|------|-------|------------|----------|----------|
| ASan | 2x slower | Yes | Linux, macOS, Windows | Buffer overflows, use-after-free |
| LSan | Minimal | Yes | Linux, macOS | Memory leaks |
| UBSan | Minimal | Yes | Linux, macOS, Windows | Undefined behavior |
| TSan | 5-15x slower | Yes | Linux, macOS | Data races |
| Valgrind | 10-50x slower | No | Linux, macOS | Everything (slow) |
| Dr. Memory | 10x slower | No | Windows, Linux | Windows memory bugs |

---

# Static Analysis

Catch bugs without running the program.

---

## Clang-Tidy

Linter and static analyzer from the LLVM project.

### Usage

```bash
clang-tidy source.cpp -- -std=c++23

# With compile_commands.json
clang-tidy -p build/ source.cpp
```

### Useful Checks

```bash
clang-tidy -checks='-*,bugprone-*,cert-*,cppcoreguidelines-*,modernize-*' source.cpp
```

| Check Group | Purpose |
|-------------|---------|
| `bugprone-*` | Common bug patterns |
| `cert-*` | CERT secure coding guidelines |
| `cppcoreguidelines-*` | C++ Core Guidelines |
| `modernize-*` | Suggest modern C++ features |
| `readability-*` | Code clarity |
| `performance-*` | Performance issues |

### Configuration File

Create `.clang-tidy` in project root:
```yaml
Checks: >
  -*,
  bugprone-*,
  cert-*,
  modernize-*,
  -modernize-use-trailing-return-type

WarningsAsErrors: ''
HeaderFilterRegex: '.*'
```

---

## Cppcheck

Open-source static analyzer. No compilation needed.

### Usage

```bash
cppcheck --enable=all --std=c++23 src/

# Suppress noise
cppcheck --enable=all --suppress=missingIncludeSystem src/
```

### Checks

| Flag | Checks |
|------|--------|
| `--enable=warning` | Suggestions about potential bugs |
| `--enable=style` | Style/readability issues |
| `--enable=performance` | Performance suggestions |
| `--enable=portability` | Portability issues |
| `--enable=all` | Everything |

### Inline Suppressions

```cpp
// cppcheck-suppress uninitvar
int x;
use(x);
```

---

## Commercial Tools

| Tool | Notes |
|------|-------|
| **PVS-Studio** | Deep analysis, free for open source |
| **Coverity** | Industry standard, free for open source via Scan |
| **SonarQube** | CI integration, quality gates |
| **Polyspace** | Formal methods, automotive/aerospace |

---

# Quick Reference

## Compile-Time

| Feature | Purpose | Example |
|---------|---------|---------|
| `[[nodiscard]]` | Warn on ignored return | Error codes, malloc |
| `[[maybe_unused]]` | Suppress unused warning | Debug functions |
| `[[deprecated]]` | Mark old API | Legacy functions |
| `[[noreturn]]` | Never returns | exit(), fatal() |
| `static_assert` | Compile-time check | Struct sizes |
| `nullptr` | Type-safe null | Pointer init |
| `constexpr` | Compile-time constant | Lookup tables |
| `consteval` | Must be compile-time | Forced const eval |
| `const` | Read-only data | Function params |
| `restrict` | No aliasing promise | Optimization |

## Sanitizers

| Sanitizer | Flag | Catches |
|-----------|------|---------|
| ASan | `-fsanitize=address` | Buffer overflow, use-after-free |
| LSan | `-fsanitize=leak` | Memory leaks |
| UBSan | `-fsanitize=undefined` | Undefined behavior |
| TSan | `-fsanitize=thread` | Data races |

## Valgrind

```bash
valgrind --leak-check=full ./program
```

---

# Checklist

## New Code

- [ ] All error-returning functions have `[[nodiscard]]`
- [ ] Struct sizes validated with `static_assert` (if ABI-dependent)
- [ ] Using `nullptr` instead of `NULL`
- [ ] Lookup tables are `constexpr`
- [ ] Read-only parameters are `const`
- [ ] Non-overlapping buffers use `restrict`
- [ ] Building with `-Wall -Wextra -Wpedantic`

## Before Release

- [ ] Tested with ASan (`-fsanitize=address`)
- [ ] Tested with UBSan (`-fsanitize=undefined`)
- [ ] No leaks in LSan/Valgrind
- [ ] Multithreaded code tested with TSan
- [ ] Static analysis clean (clang-tidy, cppcheck)

## CI Pipeline

```makefile
# Debug build with sanitizers
debug:
    $(CXX) -std=c++23 -g -O1 \
        -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        -Wall -Wextra -Wpedantic -Werror \
        $(SOURCES) -o $(TARGET)

# Static analysis
lint:
    clang-tidy -p build/ $(SOURCES)
    cppcheck --enable=all --error-exitcode=1 $(SOURCES)
```
