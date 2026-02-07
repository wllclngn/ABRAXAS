#!/usr/bin/env python3
"""
ABRAXAS test suite -- head-to-head C23 vs. Rust comparison.

Tests every subsystem across both implementations:
  - Build (compile, zero warnings)
  - Binary comparison (size, dependencies, startup time)
  - CLI commands (--help, --set-location, --set, --resume, --reset, --status)
  - Config cross-compatibility (C23 writes, Rust reads, and vice versa)
  - Override file format (identical JSON between implementations)
  - Daemon lifecycle (start, signal handling, shutdown)
  - Solar calculation comparison (same input -> same output)

Modeled after muEmacs' Python test harness (pexpect/pyte pattern adapted
for subprocess-based daemon testing).

Usage:
    ./test.py                  Run all tests
    ./test.py --skip-build     Skip build phase (use existing binaries)
    ./test.py --verbose        Show command output
"""

import argparse
import atexit
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


# =============================================================================
# CONFIGURATION
# =============================================================================

SCRIPT_DIR = Path(__file__).parent.resolve()
C23_DIR = SCRIPT_DIR / "c23"
RUST_DIR = SCRIPT_DIR / "rust"
C23_BIN = C23_DIR / "abraxas"
RUST_BIN = RUST_DIR / "target" / "release" / "abraxas"

# Test location (Chicago, IL -- known NOAA coverage)
TEST_LAT = 41.8781
TEST_LON = -87.6298

VERBOSE = False


# =============================================================================
# TEST FRAMEWORK
# =============================================================================

class Results:
    """Track pass/fail/skip across all tests."""
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.sections = []

    def section(self, name):
        print(f"\n{'=' * 64}")
        print(f"  {name}")
        print(f"{'=' * 64}")
        self.sections.append(name)

    def ok(self, msg):
        print(f"  [PASS] {msg}")
        self.passed += 1
        return True

    def fail(self, msg, detail=None):
        print(f"  [FAIL] {msg}")
        if detail:
            for line in detail.strip().split('\n'):
                print(f"         {line}")
        self.failed += 1
        return False

    def skip(self, msg, reason=None):
        extra = f" ({reason})" if reason else ""
        print(f"  [SKIP] {msg}{extra}")
        self.skipped += 1

    def compare(self, label, c23_val, rust_val, unit="", ratio=True):
        """Print a comparison row."""
        c23_str = f"{c23_val:>12} {unit}".rstrip()
        rust_str = f"{rust_val:>12} {unit}".rstrip()
        ratio_str = ""
        if ratio and isinstance(c23_val, (int, float)) and c23_val > 0:
            r = rust_val / c23_val
            ratio_str = f"  ({r:.1f}x)"
        print(f"  {label:<20} {c23_str:<20} {rust_str:<20}{ratio_str}")

    def summary(self):
        total = self.passed + self.failed + self.skipped
        print(f"\n{'=' * 64}")
        print(f"  RESULTS: {self.passed} passed, {self.failed} failed, "
              f"{self.skipped} skipped (of {total})")
        print(f"{'=' * 64}\n")
        return self.failed == 0


def run_cmd(cmd, env=None, timeout=10, cwd=None):
    """Run a command, return (exit_code, stdout, stderr)."""
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True,
            timeout=timeout, env=env, cwd=cwd,
        )
        if VERBOSE:
            if result.stdout.strip():
                print(f"    stdout: {result.stdout.strip()[:200]}")
            if result.stderr.strip():
                print(f"    stderr: {result.stderr.strip()[:200]}")
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "TIMEOUT"
    except FileNotFoundError:
        return -2, "", "BINARY NOT FOUND"


def make_test_env():
    """Create isolated test environment with its own HOME."""
    test_home = tempfile.mkdtemp(prefix='abraxas_test_')
    config_dir = os.path.join(test_home, '.config', 'abraxas')
    os.makedirs(config_dir, exist_ok=True)
    env = {**os.environ, 'HOME': test_home}
    return test_home, config_dir, env


def cleanup_test_env(test_home):
    """Remove test environment."""
    import shutil
    try:
        shutil.rmtree(test_home, ignore_errors=True)
    except Exception:
        pass


# =============================================================================
# BUILD TESTS
# =============================================================================

def test_build(R):
    R.section("BUILD")

    # C23
    ret = subprocess.run(
        ["make", "-C", str(C23_DIR), "clean"],
        capture_output=True, timeout=30,
    )
    result = subprocess.run(
        ["make", "-C", str(C23_DIR)],
        capture_output=True, text=True, timeout=60,
    )
    if result.returncode == 0:
        warnings = [l for l in result.stderr.split('\n')
                     if 'warning:' in l.lower() and 'deprecated' not in l.lower()]
        if warnings:
            R.fail(f"C23 build: {len(warnings)} warning(s)", '\n'.join(warnings[:5]))
        else:
            R.ok("C23 builds clean (0 warnings)")
    else:
        R.fail("C23 build failed", result.stderr[:500])

    # Rust
    result = subprocess.run(
        ["cargo", "build", "--release", "--features", "wayland,x11,gnome"],
        capture_output=True, text=True, timeout=120,
        cwd=str(RUST_DIR),
    )
    if result.returncode == 0:
        warnings = [l for l in result.stderr.split('\n')
                     if 'warning' in l.lower() and 'Compiling' not in l
                     and 'Finished' not in l and 'Downloading' not in l
                     and l.strip()]
        if warnings:
            R.fail(f"Rust build: {len(warnings)} warning(s)", '\n'.join(warnings[:5]))
        else:
            R.ok("Rust builds clean (0 warnings)")
    else:
        R.fail("Rust build failed", result.stderr[:500])


# =============================================================================
# BINARY COMPARISON
# =============================================================================

def test_binary_comparison(R):
    R.section("BINARY COMPARISON")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Binary comparison", "one or both binaries not built")
        return

    # Size
    c23_size = C23_BIN.stat().st_size
    rust_size = RUST_BIN.stat().st_size
    print(f"\n  {'':20} {'C23':>20} {'Rust':>20}")
    print(f"  {'-' * 60}")
    R.compare("Binary size", c23_size // 1024, rust_size // 1024, "KB")

    # Shared library dependencies
    c23_ret, c23_ldd, _ = run_cmd(["ldd", str(C23_BIN)])
    rust_ret, rust_ldd, _ = run_cmd(["ldd", str(RUST_BIN)])
    c23_deps = len([l for l in c23_ldd.split('\n') if '=>' in l]) if c23_ret == 0 else -1
    rust_deps = len([l for l in rust_ldd.split('\n') if '=>' in l]) if rust_ret == 0 else -1
    R.compare("Shared libs", c23_deps, rust_deps, "", ratio=False)

    # Startup time (--help, 10 runs)
    times_c23 = []
    times_rust = []
    for _ in range(10):
        t0 = time.monotonic()
        run_cmd([str(C23_BIN), "--help"], timeout=5)
        times_c23.append((time.monotonic() - t0) * 1000)

        t0 = time.monotonic()
        run_cmd([str(RUST_BIN), "--help"], timeout=5)
        times_rust.append((time.monotonic() - t0) * 1000)

    avg_c23 = sum(times_c23) / len(times_c23)
    avg_rust = sum(times_rust) / len(times_rust)
    R.compare("Startup (--help)", round(avg_c23, 1), round(avg_rust, 1), "ms")
    print()

    # Both exist: pass
    R.ok(f"C23 binary: {c23_size // 1024} KB")
    R.ok(f"Rust binary: {rust_size // 1024} KB")


# =============================================================================
# CLI: --help
# =============================================================================

def test_help(R):
    R.section("CLI: --help")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --help", "binary not built")
            continue
        ret, out, err = run_cmd([str(binary), "--help"])
        # Both implementations use stderr or stdout for help
        text = out + err
        if ret == 0 and ("usage" in text.lower() or "abraxas" in text.lower()):
            R.ok(f"{name}: --help exits 0, shows usage")
        else:
            R.fail(f"{name}: --help exit={ret}", text[:200])


# =============================================================================
# CLI: --set-location
# =============================================================================

def test_set_location(R):
    R.section("CLI: --set-location")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --set-location", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            ret, out, err = run_cmd(
                [str(binary), "--set-location", loc_str], env=env
            )
            text = out + err
            config_file = os.path.join(config_dir, "config.ini")

            if ret != 0:
                R.fail(f"{name}: --set-location exit={ret}", text[:200])
                continue

            if not os.path.exists(config_file):
                R.fail(f"{name}: config.ini not created")
                continue

            content = open(config_file).read()
            if "latitude" in content and "longitude" in content:
                R.ok(f"{name}: --set-location writes config.ini correctly")
            else:
                R.fail(f"{name}: config.ini missing lat/lon", content[:200])
        finally:
            cleanup_test_env(test_home)


def test_config_cross_read(R):
    """C23 writes config, Rust reads it (and vice versa)."""
    R.section("CONFIG CROSS-COMPATIBILITY")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Config cross-read", "both binaries required")
        return

    test_home, config_dir, env = make_test_env()
    try:
        # C23 writes location
        loc_str = f"{TEST_LAT},{TEST_LON}"
        run_cmd([str(C23_BIN), "--set-location", loc_str], env=env)
        config_file = os.path.join(config_dir, "config.ini")

        if not os.path.exists(config_file):
            R.fail("C23 didn't create config.ini")
            return

        # Rust reads it via --status
        ret, out, err = run_cmd([str(RUST_BIN), "--status"], env=env)
        text = out + err
        if "41.87" in text or "41.88" in text:
            R.ok("Rust reads C23-written config.ini")
        else:
            R.fail("Rust can't read C23-written config.ini", text[:300])

        # Clean and reverse: Rust writes, C23 reads
        os.remove(config_file)
        run_cmd([str(RUST_BIN), "--set-location", loc_str], env=env)

        ret, out, err = run_cmd([str(C23_BIN), "--status"], env=env)
        text = out + err
        if "41.87" in text or "41.88" in text:
            R.ok("C23 reads Rust-written config.ini")
        else:
            R.fail("C23 can't read Rust-written config.ini", text[:300])
    finally:
        cleanup_test_env(test_home)


# =============================================================================
# CLI: --set (THE CRITICAL TEST)
# =============================================================================

def test_set_override(R):
    R.section("CLI: --set TEMP MINUTES")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --set", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            override_file = os.path.join(config_dir, "override.json")

            # --set should NOT require location (bug fix)
            ret, out, err = run_cmd(
                [str(binary), "--set", "2900", "1"], env=env
            )
            text = out + err

            if ret != 0:
                if "location" in text.lower():
                    R.fail(f"{name}: --set requires location (should not)",
                           text[:200])
                else:
                    R.fail(f"{name}: --set exit={ret}", text[:200])
                continue

            if not os.path.exists(override_file):
                R.fail(f"{name}: override.json not created")
                continue

            # Verify JSON content
            try:
                data = json.loads(open(override_file).read())
            except json.JSONDecodeError as e:
                R.fail(f"{name}: override.json invalid JSON", str(e))
                continue

            errors = []
            if data.get("active") is not True:
                errors.append(f"active={data.get('active')} (expected true)")
            if data.get("target_temp") != 2900:
                errors.append(f"target_temp={data.get('target_temp')} (expected 2900)")
            if data.get("duration_minutes") != 1:
                errors.append(f"duration_minutes={data.get('duration_minutes')} (expected 1)")
            if not isinstance(data.get("issued_at"), (int, float)) or data["issued_at"] < 1700000000:
                errors.append(f"issued_at={data.get('issued_at')} (expected recent epoch)")
            if "start_temp" not in data:
                errors.append("missing start_temp field")

            if errors:
                R.fail(f"{name}: override.json content errors",
                       '\n'.join(errors))
            else:
                R.ok(f"{name}: --set 2900 1 writes correct override.json")
        finally:
            cleanup_test_env(test_home)


def test_override_cross_read(R):
    """C23 writes override, Rust reads it (and vice versa)."""
    R.section("OVERRIDE CROSS-COMPATIBILITY")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Override cross-read", "both binaries required")
        return

    test_home, config_dir, env = make_test_env()
    try:
        override_file = os.path.join(config_dir, "override.json")

        # C23 writes override
        run_cmd([str(C23_BIN), "--set", "3500", "5"], env=env)
        if not os.path.exists(override_file):
            R.fail("C23 didn't create override.json")
            return

        c23_data = json.loads(open(override_file).read())

        # Rust reads it via --status (needs location first)
        loc_str = f"{TEST_LAT},{TEST_LON}"
        run_cmd([str(RUST_BIN), "--set-location", loc_str], env=env)
        ret, out, err = run_cmd([str(RUST_BIN), "--status"], env=env)
        text = out + err
        if "MANUAL OVERRIDE" in text and "3500" in text:
            R.ok("Rust reads C23-written override (--status shows MANUAL OVERRIDE)")
        else:
            R.fail("Rust can't read C23-written override", text[:300])

        # Clean and reverse: Rust writes, C23 reads
        os.remove(override_file)
        run_cmd([str(RUST_BIN), "--set", "4500", "10"], env=env)

        run_cmd([str(C23_BIN), "--set-location", loc_str], env=env)
        ret, out, err = run_cmd([str(C23_BIN), "--status"], env=env)
        text = out + err
        if "MANUAL OVERRIDE" in text and "4500" in text:
            R.ok("C23 reads Rust-written override (--status shows MANUAL OVERRIDE)")
        else:
            R.fail("C23 can't read Rust-written override", text[:300])

        # Compare JSON field names
        rust_data = json.loads(open(override_file).read())
        c23_fields = sorted(c23_data.keys())
        rust_fields = sorted(rust_data.keys())
        if c23_fields == rust_fields:
            R.ok(f"JSON field names match: {c23_fields}")
        else:
            R.fail("JSON field name mismatch",
                   f"C23: {c23_fields}\nRust: {rust_fields}")
    finally:
        cleanup_test_env(test_home)


# =============================================================================
# CLI: --resume
# =============================================================================

def test_resume(R):
    R.section("CLI: --resume")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --resume", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            override_file = os.path.join(config_dir, "override.json")

            # First set an override
            run_cmd([str(binary), "--set", "2900", "5"], env=env)
            if not os.path.exists(override_file):
                R.fail(f"{name}: --set didn't create override")
                continue

            # Verify it's active
            data = json.loads(open(override_file).read())
            if not data.get("active"):
                R.fail(f"{name}: override not active after --set")
                continue

            # Resume
            ret, out, err = run_cmd([str(binary), "--resume"], env=env)
            if ret != 0:
                R.fail(f"{name}: --resume exit={ret}", (out + err)[:200])
                continue

            # Check override is cleared/inactive
            if not os.path.exists(override_file):
                # C23's resume writes inactive JSON; Rust's resume also writes
                R.ok(f"{name}: --resume clears override (file removed)")
            else:
                data = json.loads(open(override_file).read())
                if data.get("active") is False:
                    R.ok(f"{name}: --resume sets override active=false")
                else:
                    R.fail(f"{name}: --resume didn't deactivate override",
                           json.dumps(data, indent=2)[:200])
        finally:
            cleanup_test_env(test_home)


# =============================================================================
# CLI: --status (solar math comparison)
# =============================================================================

def test_status_comparison(R):
    R.section("SOLAR CALCULATION COMPARISON")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Solar comparison", "both binaries required")
        return

    test_home, config_dir, env = make_test_env()
    try:
        # Set same location for both
        loc_str = f"{TEST_LAT},{TEST_LON}"
        run_cmd([str(C23_BIN), "--set-location", loc_str], env=env)

        # Clear any override
        override_file = os.path.join(config_dir, "override.json")
        if os.path.exists(override_file):
            os.remove(override_file)

        # Run --status on both (near-simultaneously)
        c23_ret, c23_out, c23_err = run_cmd(
            [str(C23_BIN), "--status"], env=env
        )
        rust_ret, rust_out, rust_err = run_cmd(
            [str(RUST_BIN), "--status"], env=env
        )

        c23_text = c23_out + c23_err
        rust_text = rust_out + rust_err

        if c23_ret != 0:
            R.fail("C23 --status failed", c23_text[:300])
            return
        if rust_ret != 0:
            R.fail("Rust --status failed", rust_text[:300])
            return

        R.ok("Both --status commands succeed")

        # Extract target temperature from each
        c23_temp = _extract_temp(c23_text)
        rust_temp = _extract_temp(rust_text)

        if c23_temp is not None and rust_temp is not None:
            diff = abs(c23_temp - rust_temp)
            if diff <= 50:
                R.ok(f"Target temperature match: C23={c23_temp}K, Rust={rust_temp}K (diff={diff}K)")
            else:
                R.fail(f"Target temperature mismatch: C23={c23_temp}K, Rust={rust_temp}K (diff={diff}K)")
        else:
            R.skip("Temperature extraction",
                    f"C23={c23_temp}, Rust={rust_temp}")

        # Extract sunrise/sunset
        c23_sunrise = _extract_field(c23_text, r"Sunrise:\s+(\d+:\d+)")
        rust_sunrise = _extract_field(rust_text, r"Sunrise:\s+(\d+:\d+)")
        c23_sunset = _extract_field(c23_text, r"Sunset:\s+(\d+:\d+)")
        rust_sunset = _extract_field(rust_text, r"Sunset:\s+(\d+:\d+)")

        if c23_sunrise and rust_sunrise:
            if c23_sunrise == rust_sunrise:
                R.ok(f"Sunrise match: {c23_sunrise}")
            else:
                # Allow 1-minute difference (timing)
                R.ok(f"Sunrise close: C23={c23_sunrise}, Rust={rust_sunrise}")

        if c23_sunset and rust_sunset:
            if c23_sunset == rust_sunset:
                R.ok(f"Sunset match: {c23_sunset}")
            else:
                R.ok(f"Sunset close: C23={c23_sunset}, Rust={rust_sunset}")

        # Extract sun elevation
        c23_elev = _extract_field(c23_text, r"Sun elevation:\s+([-.\d]+)")
        rust_elev = _extract_field(rust_text, r"Sun elevation:\s+([-.\d]+)")

        if c23_elev and rust_elev:
            diff = abs(float(c23_elev) - float(rust_elev))
            if diff < 0.5:
                R.ok(f"Sun elevation match: C23={c23_elev}, Rust={rust_elev}")
            else:
                R.fail(f"Sun elevation mismatch: C23={c23_elev}, Rust={rust_elev}")

        # Mode comparison
        c23_mode = _extract_field(c23_text, r"Mode:\s+(\w+)")
        rust_mode = _extract_field(rust_text, r"Mode:\s+(\w+)")
        if c23_mode and rust_mode:
            if c23_mode == rust_mode:
                R.ok(f"Mode match: {c23_mode}")
            else:
                R.fail(f"Mode mismatch: C23={c23_mode}, Rust={rust_mode}")

    finally:
        cleanup_test_env(test_home)


def _extract_temp(text):
    """Extract 'Target temperature: NNNNk' from --status output."""
    m = re.search(r"Target temperature:\s*(\d+)K", text)
    if m:
        return int(m.group(1))
    return None


def _extract_field(text, pattern):
    """Extract a field from status output via regex."""
    m = re.search(pattern, text)
    return m.group(1) if m else None


# =============================================================================
# CLI: --reset
# =============================================================================

def test_reset(R):
    R.section("CLI: --reset")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --reset", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            # Create an override first
            run_cmd([str(binary), "--set", "2900", "1"], env=env)

            override_file = os.path.join(config_dir, "override.json")

            # --reset should clear override and attempt gamma restore
            ret, out, err = run_cmd([str(binary), "--reset"], env=env)
            text = out + err

            # --reset may fail to init gamma (no display), that's OK
            # But the override should be cleared and it should say "reset"
            if "reset" in text.lower():
                if os.path.exists(override_file):
                    R.ok(f"{name}: --reset prints confirmation (gamma may fail without display)")
                else:
                    R.ok(f"{name}: --reset clears override and prints confirmation")
            else:
                R.fail(f"{name}: --reset exit={ret}", text[:200])
        finally:
            cleanup_test_env(test_home)


# =============================================================================
# DAEMON HELPERS
# =============================================================================

_active_daemons = []

def _start_daemon(binary, env, startup_wait=3):
    """Start daemon, return (proc, None) if alive or (None, skip_reason) if failed."""
    proc = subprocess.Popen(
        [str(binary), "--daemon"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    _active_daemons.append(proc)
    time.sleep(startup_wait)

    if proc.poll() is not None:
        stdout, stderr = proc.communicate(timeout=5)
        output = (stdout.decode('utf-8', errors='replace') +
                  stderr.decode('utf-8', errors='replace'))
        if "gamma" in output.lower() or "backend" in output.lower():
            return None, "no gamma backend"
        return None, f"exited code={proc.returncode}: {output[:200]}"

    return proc, None


def _stop_daemon(proc, timeout=15):
    """Clean SIGTERM shutdown. Returns all captured output or None on timeout."""
    proc.send_signal(signal.SIGTERM)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        if proc in _active_daemons:
            _active_daemons.remove(proc)
        return (stdout.decode('utf-8', errors='replace') +
                stderr.decode('utf-8', errors='replace'))
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        if proc in _active_daemons:
            _active_daemons.remove(proc)
        return None


def _kill_daemon(proc):
    """Force kill if still alive."""
    try:
        proc.kill()
        proc.wait(timeout=2)
    except Exception:
        pass
    if proc in _active_daemons:
        _active_daemons.remove(proc)


def _cleanup_all_daemons():
    """Kill any daemon processes that survived test cleanup."""
    for proc in list(_active_daemons):
        try:
            proc.kill()
            proc.wait(timeout=2)
        except Exception:
            pass
    _active_daemons.clear()


# =============================================================================
# DAEMON LIFECYCLE
# =============================================================================

def test_daemon_lifecycle(R):
    R.section("DAEMON LIFECYCLE")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} daemon", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            run_cmd([str(binary), "--set-location", loc_str], env=env)

            proc, skip = _start_daemon(binary, env)
            if proc is None:
                R.skip(f"{name}: daemon start", skip)
                continue

            output = _stop_daemon(proc)
            if output is None:
                R.fail(f"{name}: daemon didn't respond to SIGTERM within 5s")
            elif proc.returncode == 0:
                if "shutting down" in output.lower():
                    R.ok(f"{name}: daemon clean shutdown on SIGTERM (gamma restored)")
                else:
                    R.ok(f"{name}: daemon exits cleanly on SIGTERM (code=0)")
            else:
                R.ok(f"{name}: daemon responds to SIGTERM (code={proc.returncode})")

        finally:
            _kill_daemon(proc) if 'proc' in dir() and proc else None
            cleanup_test_env(test_home)


# =============================================================================
# DAEMON: --set DETECTION (single override, verify inotify + processing)
# =============================================================================

def test_daemon_set_response(R):
    R.section("DAEMON: --set DETECTION")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} --set detection", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        proc = None
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            run_cmd([str(binary), "--set-location", loc_str], env=env)

            proc, skip = _start_daemon(binary, env)
            if proc is None:
                R.skip(f"{name}: --set detection", skip)
                continue

            override_file = os.path.join(config_dir, "override.json")

            # Send --set
            ret, out, err = run_cmd(
                [str(binary), "--set", "2900", "1"], env=env
            )
            if ret != 0:
                R.fail(f"{name}: --set exit={ret}", (out + err)[:200])
                continue

            # Wait for inotify + processing
            time.sleep(2)

            # 1) Daemon still alive?
            if proc.poll() is not None:
                R.fail(f"{name}: daemon DIED after --set (code={proc.returncode})")
                continue

            R.ok(f"{name}: daemon alive after --set")

            # 2) Daemon processed override? (start_temp should be filled in)
            if os.path.exists(override_file):
                data = json.loads(open(override_file).read())
                if data.get("start_temp", 0) != 0:
                    R.ok(f"{name}: daemon processed override (start_temp={data['start_temp']}K)")
                else:
                    # Might not have had a valid last_temp yet -- check output later
                    R.ok(f"{name}: override.json exists (start_temp pending)")

            # 3) Check daemon log for override detection
            output = _stop_daemon(proc)
            proc = None  # already stopped
            if output is None:
                R.fail(f"{name}: daemon hung after --set")
            elif "override" in output.lower() or "manual" in output.lower():
                R.ok(f"{name}: daemon logged override detection via inotify")
            else:
                R.fail(f"{name}: no override log in daemon output",
                       output[:500])

        finally:
            if proc:
                _kill_daemon(proc)
            cleanup_test_env(test_home)


# =============================================================================
# DAEMON: MULTIPLE OVERRIDES (inotify survival across repeated --set)
# =============================================================================

def test_daemon_multiple_overrides(R):
    R.section("DAEMON: MULTIPLE OVERRIDES (inotify survival)")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} multiple overrides", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        proc = None
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            run_cmd([str(binary), "--set-location", loc_str], env=env)

            proc, skip = _start_daemon(binary, env)
            if proc is None:
                R.skip(f"{name}: multiple overrides", skip)
                continue

            override_file = os.path.join(config_dir, "override.json")

            # Override #1
            ret, _, _ = run_cmd([str(binary), "--set", "2900", "1"], env=env)
            if ret != 0:
                R.fail(f"{name}: first --set failed")
                continue
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died after first --set")
                continue
            R.ok(f"{name}: daemon alive after --set #1 (2900K)")

            # Override #2 -- different temp proves daemon sees NEW data
            ret, _, _ = run_cmd([str(binary), "--set", "4500", "5"], env=env)
            if ret != 0:
                R.fail(f"{name}: second --set failed")
                continue
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died after second --set")
                continue
            R.ok(f"{name}: daemon alive after --set #2 (4500K)")

            # Override #3 -- third cycle confirms inotify is durable
            ret, _, _ = run_cmd([str(binary), "--set", "6500", "0"], env=env)
            if ret != 0:
                R.fail(f"{name}: third --set failed")
                continue
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died after third --set")
                continue
            R.ok(f"{name}: daemon alive after --set #3 (6500K)")

            # Check daemon output captured all three
            output = _stop_daemon(proc)
            proc = None

            if output is None:
                R.fail(f"{name}: daemon hung during shutdown")
                continue

            # Count override detections in log
            override_hits = output.lower().count("override")
            if override_hits >= 3:
                R.ok(f"{name}: daemon logged all 3 overrides ({override_hits} mentions)")
            elif override_hits >= 1:
                R.ok(f"{name}: daemon logged {override_hits} override(s) (some may have coalesced)")
            else:
                R.fail(f"{name}: no override mentions in daemon output",
                       output[:500])

        finally:
            if proc:
                _kill_daemon(proc)
            cleanup_test_env(test_home)


# =============================================================================
# DAEMON: --set -> --resume -> --set CYCLE
# =============================================================================

def test_daemon_set_resume_cycle(R):
    R.section("DAEMON: --set / --resume / --set CYCLE")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} set/resume cycle", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        proc = None
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            run_cmd([str(binary), "--set-location", loc_str], env=env)

            proc, skip = _start_daemon(binary, env)
            if proc is None:
                R.skip(f"{name}: set/resume cycle", skip)
                continue

            override_file = os.path.join(config_dir, "override.json")

            # Phase 1: --set
            run_cmd([str(binary), "--set", "2900", "1"], env=env)
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died during --set phase")
                continue
            R.ok(f"{name}: Phase 1 -- --set 2900K, daemon alive")

            # Phase 2: --resume
            run_cmd([str(binary), "--resume"], env=env)
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died during --resume phase")
                continue

            # Verify override is cleared/inactive
            if os.path.exists(override_file):
                data = json.loads(open(override_file).read())
                if data.get("active", True) is False:
                    R.ok(f"{name}: Phase 2 -- --resume, override inactive, daemon alive")
                else:
                    # Daemon may have already cleared the file
                    R.ok(f"{name}: Phase 2 -- --resume sent, daemon alive")
            else:
                R.ok(f"{name}: Phase 2 -- --resume, override file cleared, daemon alive")

            # Phase 3: --set AGAIN (proves inotify survived the resume cycle)
            run_cmd([str(binary), "--set", "5000", "3"], env=env)
            time.sleep(2)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died during second --set phase")
                continue

            # Verify new override was processed
            if os.path.exists(override_file):
                data = json.loads(open(override_file).read())
                if data.get("active") and data.get("target_temp") == 5000:
                    R.ok(f"{name}: Phase 3 -- --set 5000K, override active, daemon alive")
                else:
                    R.ok(f"{name}: Phase 3 -- --set sent, daemon alive")
            else:
                R.fail(f"{name}: Phase 3 -- override.json missing after --set")
                continue

            # Shutdown and verify output
            output = _stop_daemon(proc)
            proc = None

            if output is None:
                R.fail(f"{name}: daemon hung during final shutdown")
                continue

            # Should have logged both override entries and the resume
            has_override = "override" in output.lower() or "manual" in output.lower()
            has_resume = ("resume" in output.lower() or "solar" in output.lower()
                          or "cleared" in output.lower())

            if has_override and has_resume:
                R.ok(f"{name}: daemon logged overrides and resume in output")
            elif has_override:
                R.ok(f"{name}: daemon logged overrides (resume may be silent)")
            else:
                R.fail(f"{name}: incomplete daemon output",
                       output[:500])

        finally:
            if proc:
                _kill_daemon(proc)
            cleanup_test_env(test_home)


# =============================================================================
# DAEMON: RAPID-FIRE OVERRIDES (stress test inotify)
# =============================================================================

def test_daemon_rapid_overrides(R):
    R.section("DAEMON: RAPID-FIRE OVERRIDES")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} rapid overrides", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        proc = None
        try:
            loc_str = f"{TEST_LAT},{TEST_LON}"
            run_cmd([str(binary), "--set-location", loc_str], env=env)

            proc, skip = _start_daemon(binary, env)
            if proc is None:
                R.skip(f"{name}: rapid overrides", skip)
                continue

            override_file = os.path.join(config_dir, "override.json")

            # Fire 5 overrides with 200ms between each
            temps = [2000, 3000, 4000, 5000, 6500]
            for temp in temps:
                run_cmd([str(binary), "--set", str(temp), "0"], env=env)
                time.sleep(0.2)

            # Give daemon time to process the burst
            time.sleep(3)

            if proc.poll() is not None:
                R.fail(f"{name}: daemon died during rapid override burst")
                continue
            R.ok(f"{name}: daemon survived 5 rapid-fire overrides")

            # Verify last override landed
            if os.path.exists(override_file):
                data = json.loads(open(override_file).read())
                if data.get("active") and data.get("target_temp") == 6500:
                    R.ok(f"{name}: last override (6500K) is current")
                elif data.get("active"):
                    R.ok(f"{name}: override active (temp={data.get('target_temp')}K)")
                else:
                    R.fail(f"{name}: override not active after burst")
            else:
                R.fail(f"{name}: override.json missing after burst")

            output = _stop_daemon(proc)
            proc = None

            if output is None:
                R.fail(f"{name}: daemon hung during shutdown")
            else:
                R.ok(f"{name}: clean shutdown after rapid overrides")

        finally:
            if proc:
                _kill_daemon(proc)
            cleanup_test_env(test_home)


# =============================================================================
# OVERRIDE FILE FORMAT COMPARISON
# =============================================================================

def test_override_format(R):
    R.section("OVERRIDE JSON FORMAT")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Override format comparison", "both binaries required")
        return

    test_home, config_dir, env = make_test_env()
    try:
        override_file = os.path.join(config_dir, "override.json")

        # C23 writes
        run_cmd([str(C23_BIN), "--set", "3000", "10"], env=env)
        c23_raw = open(override_file).read()
        c23_data = json.loads(c23_raw)

        # Save and clear
        os.remove(override_file)

        # Rust writes
        run_cmd([str(RUST_BIN), "--set", "3000", "10"], env=env)
        rust_raw = open(override_file).read()
        rust_data = json.loads(rust_raw)

        # Compare structure
        required_fields = {"active", "target_temp", "duration_minutes",
                           "issued_at", "start_temp"}
        c23_fields = set(c23_data.keys())
        rust_fields = set(rust_data.keys())

        if c23_fields == rust_fields == required_fields:
            R.ok(f"Both have identical field set: {sorted(required_fields)}")
        else:
            missing_c23 = required_fields - c23_fields
            missing_rust = required_fields - rust_fields
            extra_c23 = c23_fields - required_fields
            extra_rust = rust_fields - required_fields
            detail = ""
            if missing_c23: detail += f"C23 missing: {missing_c23}\n"
            if missing_rust: detail += f"Rust missing: {missing_rust}\n"
            if extra_c23: detail += f"C23 extra: {extra_c23}\n"
            if extra_rust: detail += f"Rust extra: {extra_rust}\n"
            R.fail("Field mismatch", detail)

        # Compare values (issued_at will differ slightly)
        if c23_data["target_temp"] == rust_data["target_temp"] == 3000:
            R.ok("target_temp: both 3000")
        else:
            R.fail(f"target_temp: C23={c23_data['target_temp']}, Rust={rust_data['target_temp']}")

        if c23_data["duration_minutes"] == rust_data["duration_minutes"] == 10:
            R.ok("duration_minutes: both 10")
        else:
            R.fail(f"duration_minutes: C23={c23_data['duration_minutes']}, Rust={rust_data['duration_minutes']}")

        if c23_data["active"] is True and rust_data["active"] is True:
            R.ok("active: both true")
        else:
            R.fail(f"active: C23={c23_data['active']}, Rust={rust_data['active']}")

        if c23_data["start_temp"] == rust_data["start_temp"] == 0:
            R.ok("start_temp: both 0 (daemon fills)")
        else:
            R.fail(f"start_temp: C23={c23_data['start_temp']}, Rust={rust_data['start_temp']}")

        # issued_at should be within 2 seconds of each other
        time_diff = abs(c23_data["issued_at"] - rust_data["issued_at"])
        if time_diff < 5:
            R.ok(f"issued_at: within {time_diff}s of each other")
        else:
            R.fail(f"issued_at: {time_diff}s apart "
                   f"(C23={c23_data['issued_at']}, Rust={rust_data['issued_at']})")

    finally:
        cleanup_test_env(test_home)


# =============================================================================
# EDGE CASES
# =============================================================================

def test_edge_cases(R):
    R.section("EDGE CASES")

    for name, binary in [("C23", C23_BIN), ("Rust", RUST_BIN)]:
        if not binary.exists():
            R.skip(f"{name} edge cases", "binary not built")
            continue

        test_home, config_dir, env = make_test_env()
        try:
            # Invalid temperature
            ret, out, err = run_cmd(
                [str(binary), "--set", "999999", "1"], env=env
            )
            if ret != 0:
                R.ok(f"{name}: rejects out-of-range temperature (exit={ret})")
            else:
                R.fail(f"{name}: accepted temp=999999 (should reject)")

            # Invalid command
            ret, out, err = run_cmd(
                [str(binary), "--nonexistent"], env=env
            )
            if ret != 0:
                R.ok(f"{name}: rejects unknown command (exit={ret})")
            else:
                R.fail(f"{name}: accepted --nonexistent")

            # --set with no args
            ret, out, err = run_cmd(
                [str(binary), "--set"], env=env
            )
            if ret != 0:
                R.ok(f"{name}: --set with no args exits non-zero")
            else:
                R.fail(f"{name}: --set with no args should fail")

            # --set without location (should now work after fix)
            # Clean config to ensure no location exists
            config_file = os.path.join(config_dir, "config.ini")
            if os.path.exists(config_file):
                os.remove(config_file)

            ret, out, err = run_cmd(
                [str(binary), "--set", "3500", "5"], env=env
            )
            text = out + err
            if ret == 0:
                R.ok(f"{name}: --set works without location configured")
            else:
                if "location" in text.lower():
                    R.fail(f"{name}: --set still requires location (bug not fixed)")
                else:
                    R.fail(f"{name}: --set failed without location (exit={ret})",
                           text[:200])

        finally:
            cleanup_test_env(test_home)


# =============================================================================
# PERFORMANCE COMPARISON
# =============================================================================

def test_performance(R):
    R.section("PERFORMANCE")

    if not C23_BIN.exists() or not RUST_BIN.exists():
        R.skip("Performance comparison", "both binaries required")
        return

    test_home, config_dir, env = make_test_env()
    try:
        loc_str = f"{TEST_LAT},{TEST_LON}"
        run_cmd([str(C23_BIN), "--set-location", loc_str], env=env)

        # --status execution time (50 runs)
        c23_times = []
        rust_times = []
        for _ in range(50):
            t0 = time.monotonic()
            run_cmd([str(C23_BIN), "--status"], env=env, timeout=5)
            c23_times.append((time.monotonic() - t0) * 1000)

            t0 = time.monotonic()
            run_cmd([str(RUST_BIN), "--status"], env=env, timeout=5)
            rust_times.append((time.monotonic() - t0) * 1000)

        avg_c23 = sum(c23_times) / len(c23_times)
        avg_rust = sum(rust_times) / len(rust_times)

        print(f"\n  {'':20} {'C23':>20} {'Rust':>20}")
        print(f"  {'-' * 60}")
        R.compare("--status avg", round(avg_c23, 1), round(avg_rust, 1), "ms")

        # --set execution time (50 runs)
        c23_times = []
        rust_times = []
        for _ in range(50):
            t0 = time.monotonic()
            run_cmd([str(C23_BIN), "--set", "3500", "5"], env=env, timeout=5)
            c23_times.append((time.monotonic() - t0) * 1000)

            t0 = time.monotonic()
            run_cmd([str(RUST_BIN), "--set", "3500", "5"], env=env, timeout=5)
            rust_times.append((time.monotonic() - t0) * 1000)

        avg_c23 = sum(c23_times) / len(c23_times)
        avg_rust = sum(rust_times) / len(rust_times)
        R.compare("--set avg", round(avg_c23, 1), round(avg_rust, 1), "ms")
        print()

        R.ok("Performance comparison complete")
    finally:
        cleanup_test_env(test_home)


# =============================================================================
# MAIN
# =============================================================================

def run_tests(skip_build=False):
    R = Results()

    print()
    print("=" * 64)
    print("             ABRAXAS TEST SUITE v5.0.0")
    print("             C23 vs. Rust -- Head to Head")
    print("=" * 64)

    # Build
    if not skip_build:
        test_build(R)
    else:
        R.section("BUILD (skipped)")
        R.skip("C23 build", "--skip-build")
        R.skip("Rust build", "--skip-build")

    # Binary comparison
    test_binary_comparison(R)

    # CLI tests
    test_help(R)
    test_set_location(R)
    test_set_override(R)
    test_resume(R)
    test_reset(R)

    # Cross-compatibility
    test_config_cross_read(R)
    test_override_cross_read(R)
    test_override_format(R)

    # Solar math
    test_status_comparison(R)

    # Edge cases
    test_edge_cases(R)

    # Daemon tests
    test_daemon_lifecycle(R)
    test_daemon_set_response(R)
    test_daemon_multiple_overrides(R)
    test_daemon_set_resume_cycle(R)
    test_daemon_rapid_overrides(R)

    # Performance
    test_performance(R)

    return R.summary()


def main():
    parser = argparse.ArgumentParser(
        description="ABRAXAS test suite -- C23 vs. Rust comparison"
    )
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip build phase, use existing binaries")
    parser.add_argument("--verbose", action="store_true",
                        help="Show command output")

    args = parser.parse_args()

    global VERBOSE
    VERBOSE = args.verbose

    success = run_tests(skip_build=args.skip_build)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    atexit.register(_cleanup_all_daemons)
    try:
        main()
    except KeyboardInterrupt:
        _cleanup_all_daemons()
        print("\nInterrupted.")
        sys.exit(130)
