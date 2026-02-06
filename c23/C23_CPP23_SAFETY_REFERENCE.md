# C23/C++23 Safety Reference

Comprehensive guide to catching bugs at compile-time, runtime, and with tooling.

---

## Table of Contents

1. [Compile-Time Safety](#compile-time-safety)
   - Attributes, Static Assertions, Type Safety, Pointer Qualifiers, Compiler Flags
   - Checked Arithmetic, Bit Manipulation, `_Generic`, `unreachable()`
   - `[[reproducible]]`/`[[unsequenced]]`, `_Atomic`, Binary Literals
   - Compatibility Layer Pattern
2. [Runtime Crash Handling](#runtime-crash-handling)
   - Fatal Signal Handlers, Operational Signal Architecture (Flag-Based)
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
CFLAGS += -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2
CFLAGS += -Wformat-security -Wnull-dereference -Warray-bounds=2
CFLAGS += -Wdouble-promotion -Wrestrict -Wcast-align
```
- `-Wshadow` — variable shadows outer scope
- `-Wconversion` — implicit type conversions
- `-Wsign-conversion` — implicit signed/unsigned conversions
- `-Wcast-qual` — casting away const/volatile
- `-Wformat=2` — printf/scanf format string checks
- `-Wformat-security` — format strings that could be security holes
- `-Wnull-dereference` — paths that dereference null pointers
- `-Warray-bounds=2` — strict out-of-bounds array access detection
- `-Wdouble-promotion` — implicit float-to-double promotion (perf trap)
- `-Wrestrict` — violations of restrict pointer contracts
- `-Wcast-align` — casts that increase alignment requirements

### Security Hardening
```makefile
CFLAGS  += -fstack-protector-strong -fstack-clash-protection -fcf-protection
CFLAGS  += -D_FORTIFY_SOURCE=3   # Release only (requires -O1+)
LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
```
- `-fstack-protector-strong` — canary on functions with local arrays or address-taken vars
- `-fstack-clash-protection` — probe stack pages to prevent clash attacks
- `-fcf-protection` — hardware-assisted control-flow integrity (Intel CET)
- `-D_FORTIFY_SOURCE=3` — bounds-checked libc wrappers (memcpy, sprintf, etc.)
- `-Wl,-z,relro` — read-only relocations (GOT hardening)
- `-Wl,-z,now` — full RELRO, resolve all symbols at load time
- `-Wl,-z,noexecstack` — mark stack non-executable

### For CI/Release
```makefile
CFLAGS += -Werror -pedantic-errors
```
- `-Werror` — treat warnings as errors
- `-pedantic-errors` — strict ISO violations are errors, not warnings

### Full Example (Makefile)
```makefile
CFLAGS := -std=c2x -O2 -march=native -fPIC
CFLAGS += -Wall -Wextra -Wpedantic
CFLAGS += -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2
CFLAGS += -Wformat-security -Wnull-dereference -Wdouble-promotion -Wrestrict
CFLAGS += -fstack-protector-strong -fstack-clash-protection -fcf-protection
LDFLAGS += -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
# CFLAGS += -Werror  # Enable for CI
```

### Full Example (CMake)
```cmake
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

target_compile_options(${TARGET} PRIVATE
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-security -Wnull-dereference -Warray-bounds=2
    -Wconversion -Wsign-conversion -Wshadow -Wdouble-promotion -Wrestrict -Wcast-align
    -fstack-protector-strong -fstack-clash-protection -fcf-protection
    $<$<CONFIG:Release>:-O3 -march=native -D_FORTIFY_SOURCE=3>
    $<$<CONFIG:Debug>:-O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer -Werror>
)
target_link_options(${TARGET} PRIVATE
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
```

---

## Checked Integer Arithmetic (`<stdckdint.h>`)

C23 standardizes overflow-safe arithmetic. Returns `true` on overflow, stores result in
first argument. Prevents silent wraparound bugs in buffer size calculations.

```c
#include <stdckdint.h>

size_t alloc_size;
if (ckd_mul(&alloc_size, count, sizeof(struct item))) {
    // Overflow: count * sizeof(struct item) exceeds SIZE_MAX
    return -ENOMEM;
}
if (ckd_add(&alloc_size, alloc_size, header_size)) {
    return -ENOMEM;
}
void *buf = malloc(alloc_size);  // Safe
```

### Fallback for Pre-C23

GCC 5+ and Clang 3.8+ provide `__builtin_*_overflow`:

```c
#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#else
#define ckd_add(result, a, b) __builtin_add_overflow((a), (b), (result))
#define ckd_sub(result, a, b) __builtin_sub_overflow((a), (b), (result))
#define ckd_mul(result, a, b) __builtin_mul_overflow((a), (b), (result))
#endif
```

**Use cases:**
- Buffer allocation: `count * element_size`
- Array indexing: `base + offset`
- Any arithmetic near `SIZE_MAX` or `INT_MAX` boundaries

---

## Bit Manipulation (`<stdbit.h>`)

C23 standardizes common bit operations. Replaces hand-rolled or GCC-specific builtins.

```c
#include <stdbit.h>

unsigned int x = 0xFF00;
stdc_count_ones_ui(x);       // 8 (population count)
stdc_leading_zeros_ui(x);    // 16
stdc_trailing_zeros_ui(x);   // 8
stdc_has_single_bit_ui(x);   // false (not power of 2)
stdc_bit_width_ui(x);        // 16 (minimum bits needed)
stdc_bit_ceil_ui(x);         // 0x10000 (next power of 2)
stdc_bit_floor_ui(x);        // 0x8000 (prev power of 2)
```

### Fallback for Pre-C23

```c
#if __has_include(<stdbit.h>)
#include <stdbit.h>
#else
#define stdc_count_ones_ui(x)     ((unsigned)__builtin_popcount(x))
#define stdc_leading_zeros_ui(x)  ((x) ? (unsigned)__builtin_clz(x) : 32u)
#define stdc_trailing_zeros_ui(x) ((x) ? (unsigned)__builtin_ctz(x) : 32u)
#define stdc_has_single_bit_ui(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)
#endif
```

**Use cases:**
- Hash table sizing (next power of 2)
- Flag/bitfield manipulation
- Efficient lookup table indexing

---

## `_Generic` Type-Safe Macros

C11/C23 `_Generic` enables type-dispatched macros -- safer than untyped `#define`.

### Type-Safe Min/Max (No Double Evaluation)

```c
static inline int    min_int(int a, int b)       { return a < b ? a : b; }
static inline long   min_long(long a, long b)    { return a < b ? a : b; }
static inline size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

#define SAFE_MIN(a, b) _Generic((a), \
    int:    min_int,    \
    long:   min_long,   \
    size_t: min_size    \
)(a, b)

size_t n = SAFE_MIN(buffer_len, max_read);  // Dispatches to min_size
int x = SAFE_MIN(3, 5);                     // Dispatches to min_int
```

### Type-Safe Swap

```c
#define SAFE_SWAP(a, b) do {   \
    typeof(a) _tmp = (a);      \
    (a) = (b);                 \
    (b) = _tmp;                \
} while (0)
```

**Why not `(a) < (b) ? (a) : (b)`:**
- Double evaluation: `MIN(i++, j++)` increments twice
- Type mismatch: `MIN(signed_int, unsigned_size_t)` silently promotes
- `_Generic` catches type mismatches at compile-time

---

## `unreachable()` (C23)

Optimization hint for code paths that cannot execute. Defined in `<stddef.h>`.
Undefined behavior if reached -- the compiler can assume it never happens.

```c
#include <stddef.h>

enum direction { NORTH, SOUTH, EAST, WEST };

const char *dir_name(enum direction d) {
    switch (d) {
    case NORTH: return "north";
    case SOUTH: return "south";
    case EAST:  return "east";
    case WEST:  return "west";
    }
    unreachable();  // Compiler knows all cases covered, eliminates "missing return" warning
}
```

### Fallback

```c
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define unreachable() __builtin_unreachable()  // GCC/Clang
#endif
```

---

## `[[reproducible]]` and `[[unsequenced]]` (C23)

New function attributes for pure/const function optimization hints.

```c
// [[reproducible]] = same inputs -> same output (may read global state)
// Equivalent to GCC __attribute__((pure))
[[reproducible]] int string_length(const char *s);

// [[unsequenced]] = same inputs -> same output, NO side effects, NO global reads
// Equivalent to GCC __attribute__((const))
[[unsequenced]] int square(int x);
```

### Fallback

```c
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define REPRODUCIBLE [[reproducible]]
#define UNSEQUENCED  [[unsequenced]]
#elif defined(__GNUC__)
#define REPRODUCIBLE __attribute__((pure))
#define UNSEQUENCED  __attribute__((const))
#else
#define REPRODUCIBLE
#define UNSEQUENCED
#endif
```

**Use on:** Character classification, hash functions, math helpers, lookup functions.

---

## `_Atomic` Types (C11/C23)

For data shared between signal handlers and main code, or between threads.
Stronger than `volatile` -- provides actual memory ordering guarantees.

```c
#include <stdatomic.h>

struct terminal {
    _Atomic short t_nrow;   // Updated by SIGWINCH handler
    _Atomic short t_ncol;   // Read by display code
};

// Signal handler (async-signal-safe with atomics)
void handle_winch(int sig) {
    (void)sig;
    atomic_store(&term.t_nrow, new_rows);
    atomic_store(&term.t_ncol, new_cols);
}

// Main loop
void update_display(void) {
    short rows = atomic_load(&term.t_nrow);
    // ...
}
```

### `volatile sig_atomic_t` vs `_Atomic`

| Feature | `volatile sig_atomic_t` | `_Atomic` |
|---------|------------------------|-----------|
| Signal-safe | Yes | Yes (for lock-free types) |
| Thread-safe | **No** | Yes |
| Memory ordering | None | Configurable |
| Type | Integer only | Any lock-free type |

**Rule:** Use `volatile sig_atomic_t` for simple signal flags. Use `_Atomic` for
struct fields that need concurrent read/write safety (signal handlers + main loop,
or multithreaded access).

---

## Binary Literals (C23)

Direct binary notation for bit patterns. Clearer than hex for flags, masks, control codes.

```c
#define CFCPCN  0b0000000000000001   // Last command was C-P, C-N
#define CFKILL  0b0000000000000010   // Last command was a kill
#define CFYANK  0b0000000000000100   // Last command was a yank

#define BELL    0b00000111U          // ASCII BEL
#define TAB     0b00001001U          // ASCII HT
```

---

## Compatibility Layer Pattern

Real projects need to support both C23 and older compilers. Use a `c23_compat.h`
header with macro wrappers:

```c
/* c23_compat.h - C23 features with pre-C23 fallbacks */
#ifndef C23_COMPAT_H
#define C23_COMPAT_H

#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 202311L
#define HAVE_C23 1
#endif
#endif
#ifndef HAVE_C23
#define HAVE_C23 0
#endif

/* constexpr */
#if HAVE_C23
#define CONSTEXPR constexpr
#else
#define CONSTEXPR static const
#endif

/* [[nodiscard]] */
#if HAVE_C23 || defined(__GNUC__)
#define NODISCARD [[nodiscard]]
#define NODISCARD_MSG(msg) [[nodiscard(msg)]]
#else
#define NODISCARD
#define NODISCARD_MSG(msg)
#endif

/* [[noreturn]] */
#if HAVE_C23
#define NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NORETURN _Noreturn
#elif defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

/* nullptr */
#if !HAVE_C23 && !defined(__cplusplus)
#ifndef nullptr
#define nullptr ((void*)0)
#endif
#endif

/* unreachable() */
#if !HAVE_C23
#if defined(__GNUC__)
#define unreachable() __builtin_unreachable()
#else
#define unreachable() do {} while (0)
#endif
#endif

/* static_assert (C11 fallback) */
#if !HAVE_C23 && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define static_assert _Static_assert
#endif

/* Checked arithmetic */
#if HAVE_C23 && __has_include(<stdckdint.h>)
#include <stdckdint.h>
#elif defined(__GNUC__) && __GNUC__ >= 5
#define ckd_add(r, a, b) __builtin_add_overflow((a), (b), (r))
#define ckd_sub(r, a, b) __builtin_sub_overflow((a), (b), (r))
#define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif

#endif /* C23_COMPAT_H */
```

**Usage:**
```c
#include "c23_compat.h"

CONSTEXPR int BUFFER_SIZE = 4096;
NODISCARD int parse_config(const char *path);
NORETURN void fatal(const char *msg);
```

This pattern lets you write C23-idiomatic code today while maintaining compatibility.

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
- `write()`, `_exit()`, `signal()`, `tcsetattr()`
- **NOT safe:** `printf()`, `malloc()`, `free()`, `new`, most of libc

`dprintf(fd, ...)` is safe. `fprintf(stderr, ...)` is not.

---

## Operational Signal Architecture (Flag-Based)

The crash handler pattern above is for fatal signals only. For operational signals
(SIGWINCH, SIGINT, SIGHUP, SIGTERM, SIGCONT), use the flag-based pattern:
handlers set flags, main loop does real work.

### The Pattern

```c
#include <signal.h>
#include <stdatomic.h>
#include <termios.h>
#include <unistd.h>

/* Global signal flags -- only these are touched in handlers */
volatile sig_atomic_t sig_winch_pending = 0;
volatile sig_atomic_t sig_int_pending   = 0;
volatile sig_atomic_t sig_term_pending  = 0;

/* Saved terminal state for async-signal-safe restoration */
static struct termios saved_termios;
static bool termios_saved = false;

/* Handlers ONLY set flags -- no real work */
static void handle_winch(int sig) { (void)sig; sig_winch_pending = 1; }
static void handle_int(int sig)   { (void)sig; sig_int_pending = 1; }

static void handle_term(int sig) {
    (void)sig;
    sig_term_pending = 1;
    /* Terminal restoration is async-signal-safe (only write + tcsetattr) */
    restore_terminal_safe();
}

/* Async-signal-safe terminal restoration */
static void restore_terminal_safe(void) {
    static const char reset[] = "\033[0m\033[?1049l\033[?25h";
    (void)write(STDOUT_FILENO, reset, sizeof(reset) - 1);
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

void signal_handlers_init(void) {
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  /* Restart interrupted read()/write() */

    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler = handle_int;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handle_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}
```

### Main Loop Checks Flags

```c
void main_loop(void) {
    while (running) {
        /* Check signals first */
        if (sig_term_pending) {
            save_modified_buffers();
            break;
        }
        if (sig_winch_pending) {
            sig_winch_pending = 0;
            handle_resize();  /* Safe: runs in main context */
        }
        if (sig_int_pending) {
            sig_int_pending = 0;
            cancel_current_operation();
        }

        /* Normal event processing */
        process_input();
    }
}
```

### `SA_RESTART` vs `SA_RESETHAND`

| Flag | Use Case |
|------|----------|
| `SA_RESTART` | Operational signals (SIGWINCH, SIGINT). Auto-restarts interrupted `read()`/`write()` so the main loop doesn't need `EINTR` handling everywhere. |
| `SA_RESETHAND` | Crash signals (SIGSEGV, SIGABRT). One-shot: handler fires once, then resets to default. Prevents recursive crashes. |

**Rule:** Use `SA_RESTART` for signals you handle and continue. Use `SA_RESETHAND` for signals that will terminate the process.

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
| `[[fallthrough]]` | Intentional switch fallthrough | Complex parsers |
| `[[reproducible]]` | Pure function hint | String length |
| `[[unsequenced]]` | Const function hint | Math helpers |
| `static_assert` | Compile-time check | Struct sizes |
| `nullptr` | Type-safe null | Pointer init |
| `constexpr` | Compile-time constant | Lookup tables |
| `consteval` | Must be compile-time (C++) | Forced const eval |
| `const` | Read-only data | Function params |
| `restrict` | No aliasing promise | Optimization |
| `unreachable()` | Dead code hint | Exhaustive switch |
| `ckd_add/sub/mul` | Overflow-safe arithmetic | Buffer allocation |
| `_Generic` | Type-dispatched macros | Safe min/max |
| `_Atomic` | Signal/thread-safe fields | Terminal state |
| `0b` literals | Binary notation | Flags, masks |

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
- [ ] Buffer arithmetic uses `ckd_add`/`ckd_mul` (not raw `*`/`+`)
- [ ] Pure functions annotated `[[reproducible]]`/`[[unsequenced]]`
- [ ] Exhaustive switches end with `unreachable()`
- [ ] Signal-shared fields use `_Atomic` or `volatile sig_atomic_t`
- [ ] Building with `-Wall -Wextra -Wpedantic`
- [ ] Building with security hardening (`-fstack-protector-strong`, RELRO, etc.)
- [ ] Compatibility layer (`c23_compat.h`) if supporting pre-C23 compilers

## Before Release

- [ ] Tested with ASan (`-fsanitize=address`)
- [ ] Tested with UBSan (`-fsanitize=undefined`)
- [ ] No leaks in LSan/Valgrind
- [ ] Multithreaded code tested with TSan
- [ ] Static analysis clean (clang-tidy, cppcheck)
- [ ] Signal handlers are async-signal-safe (only set flags)
- [ ] Terminal restoration works on SIGTERM/SIGHUP (no printf in handlers)

## CI Pipeline

```makefile
# Debug build with sanitizers
debug:
    $(CC) -std=c2x -g -O1 \
        -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        -Wall -Wextra -Wpedantic -Werror \
        -Wconversion -Wsign-conversion -Wshadow -Wformat=2 \
        -fstack-protector-strong \
        $(SOURCES) -o $(TARGET)

# Release build with hardening
release:
    $(CC) -std=c2x -O3 -march=native \
        -Wall -Wextra -Wpedantic \
        -fstack-protector-strong -fstack-clash-protection -fcf-protection \
        -D_FORTIFY_SOURCE=3 \
        $(SOURCES) -o $(TARGET) \
        -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -s

# Static analysis
lint:
    clang-tidy -p build/ $(SOURCES)
    cppcheck --enable=all --error-exitcode=1 $(SOURCES)
```
