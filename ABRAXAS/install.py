#!/usr/bin/env python3
"""
abraxas installer

Installs ABRAXAS dynamic color temperature daemon.

Usage:
    ./install.py              # Install (default)
    ./install.py uninstall    # Remove installed files
    ./install.py status       # Show installation status
    ./install.py enable       # Enable systemd service
    ./install.py disable      # Disable systemd service
"""

import argparse
import os
import sys
import shutil
import subprocess
from pathlib import Path
from datetime import datetime


# =============================================================================
# CONFIGURATION
# =============================================================================

# Use SUDO_USER's home if running under sudo, otherwise current user's home
_real_user = os.environ.get("SUDO_USER", os.environ.get("USER"))
_real_home = Path(f"/home/{_real_user}") if _real_user else Path.home()

INSTALL_BINARY = _real_home / ".local" / "bin" / "abraxas"
INSTALL_CONFIG_DIR = _real_home / ".config" / "abraxas"
INSTALL_SERVICE = _real_home / ".config" / "systemd" / "user" / "abraxas.service"



# =============================================================================
# LOGGING
# =============================================================================

def _timestamp() -> str:
    """Get current timestamp in [HH:MM:SS] format."""
    return datetime.now().strftime("[%H:%M:%S]")


def log_info(msg: str) -> None:
    print(f"{_timestamp()} [INFO]   {msg}")


def log_warn(msg: str) -> None:
    print(f"{_timestamp()} [WARN]   {msg}")


def log_error(msg: str) -> None:
    print(f"{_timestamp()} [ERROR]  {msg}")


# =============================================================================
# COMMAND EXECUTION
# =============================================================================

def run_cmd(cmd: list, cwd: Path | None = None) -> int:
    """Run a command with real-time output to terminal."""
    print(f">>> {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    return result.returncode


def run_cmd_capture(cmd: list, cwd: Path | None = None) -> tuple[int, str, str]:
    """Run a command and capture output."""
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    return result.returncode, result.stdout, result.stderr


def get_systemctl_cmd() -> list:
    """Get base systemctl command, handling sudo user context."""
    if os.environ.get("SUDO_USER"):
        return ["systemctl", f"--machine={os.environ['SUDO_USER']}@.host", "--user"]
    return ["systemctl", "--user"]


# =============================================================================
# DEPENDENCY CHECKS
# =============================================================================

def check_x11_libs() -> bool:
    """Check if X11 libraries are available (for NVIDIA fallback)."""
    ret, _, _ = run_cmd_capture(["pkg-config", "--exists", "x11", "xrandr"])
    return ret == 0


# =============================================================================
# SERVICE MANAGEMENT
# =============================================================================

def is_service_enabled() -> bool:
    """Check if abraxas systemd service is enabled."""
    cmd = get_systemctl_cmd() + ["is-enabled", "abraxas.service"]
    ret, _, _ = run_cmd_capture(cmd)
    return ret == 0


def is_service_active() -> bool:
    """Check if abraxas systemd service is running."""
    cmd = get_systemctl_cmd() + ["is-active", "abraxas.service"]
    ret, _, _ = run_cmd_capture(cmd)
    return ret == 0


def enable_service() -> bool:
    """Enable and start abraxas service."""
    log_info("Enabling abraxas service...")

    base_cmd = get_systemctl_cmd()

    # Disable old redshift service if present
    subprocess.run(
        base_cmd + ["disable", "--now", "redshift-scheduler.service"],
        capture_output=True
    )

    # Reload daemon
    subprocess.run(base_cmd + ["daemon-reload"], capture_output=True)

    # Enable new service
    ret, _, stderr = run_cmd_capture(base_cmd + ["enable", "--now", "abraxas.service"])

    if ret == 0:
        log_info("Service enabled and started")
        return True
    else:
        log_error(f"Failed to enable service: {stderr}")
        return False


def disable_service() -> bool:
    """Disable and stop abraxas service."""
    log_info("Disabling abraxas service...")

    cmd = get_systemctl_cmd() + ["disable", "--now", "abraxas.service"]
    ret, _, stderr = run_cmd_capture(cmd)

    if ret == 0:
        log_info("Service disabled and stopped")
        return True
    else:
        log_error(f"Failed to disable service: {stderr}")
        return False


def restart_service() -> bool:
    """Restart abraxas service if running."""
    if is_service_active():
        log_info("Restarting service...")
        cmd = get_systemctl_cmd() + ["restart", "abraxas.service"]
        ret, _, _ = run_cmd_capture(cmd)
        return ret == 0
    return True


# =============================================================================
# BUILD
# =============================================================================

def build_abraxas(source_dir: Path, force: bool = False,
                  non_usa: bool = False) -> bool:
    """Build abraxas binary (statically links libmeridian)."""
    binary = source_dir / "abraxas"

    # Skip build if already exists (unless forced)
    if binary.exists() and not force:
        log_info("abraxas already built (use --rebuild to force)")
        return True

    if non_usa:
        log_info("BUILDING ABRAXAS (non-USA: weather disabled)")
    else:
        log_info("BUILDING ABRAXAS")

    # Check for make
    ret, _, _ = run_cmd_capture(["which", "make"])
    if ret != 0:
        log_error("make not found. Install build tools: sudo pacman -S base-devel")
        return False

    # Check for gcc
    ret, _, _ = run_cmd_capture(["which", "gcc"])
    if ret != 0:
        log_error("gcc not found. Install build tools: sudo pacman -S base-devel")
        return False

    # Build (top-level make builds libmeridian.a then daemon)
    make_cmd = ["make", "-C", str(source_dir)]
    if non_usa:
        make_cmd.append("NOAA=0")
    ret = run_cmd(make_cmd + ["clean"])
    ret = run_cmd(make_cmd)

    if ret != 0:
        log_error("Build failed!")
        return False

    if not binary.exists():
        log_error("Build completed but abraxas binary not found")
        return False

    log_info(f"Built: {binary}")
    return True


# =============================================================================
# COMMANDS
# =============================================================================

def cmd_install(args, source_dir: Path) -> bool:
    """Install abraxas."""
    log_info("INSTALLING ABRAXAS")

    source_binary = source_dir / "abraxas"
    source_service = source_dir / "abraxas.service"
    source_zipdb = source_dir / "us_zipcodes.bin"

    # Determine NOAA mode: --non-usa flag overrides, otherwise prompt
    if args.non_usa:
        non_usa = True
    else:
        print()
        while True:
            response = input(
                "ABRAXAS allows for utilization of US-specific NOAA data\n"
                "for weather calculations. Do you wish to enable NOAA\n"
                "data calculations? [Y/N]: "
            ).strip()
            if response in ("y", "Y"):
                non_usa = False
                break
            elif response in ("n", "N"):
                non_usa = True
                break
        print()

    # Build (always force -- user just chose NOAA config)
    if not build_abraxas(source_dir, force=True, non_usa=non_usa):
        return False

    # Create directories
    log_info("Creating directories...")
    INSTALL_BINARY.parent.mkdir(parents=True, exist_ok=True)
    INSTALL_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    INSTALL_SERVICE.parent.mkdir(parents=True, exist_ok=True)

    # Stop running abraxas before overwriting binary (ETXTBSY)
    subprocess.run(["systemctl", "--user", "stop", "abraxas"], capture_output=True)
    subprocess.run(["pkill", "-x", "abraxas"], capture_output=True)

    # Copy binary
    log_info(f"Installing {INSTALL_BINARY}...")
    shutil.copy2(source_binary, INSTALL_BINARY)
    INSTALL_BINARY.chmod(0o755)

    # Copy ZIP database (skip for non-USA builds)
    if not non_usa:
        if source_zipdb.exists():
            dest_zipdb = INSTALL_CONFIG_DIR / "us_zipcodes.bin"
            if not dest_zipdb.exists():
                log_info("Installing ZIP database...")
                shutil.copy2(source_zipdb, dest_zipdb)
            else:
                log_info("ZIP database already installed")
        else:
            log_warn("ZIP database not found in source")
    else:
        log_info("Skipping ZIP database (non-USA build)")

    # Copy systemd service
    if source_service.exists():
        log_info("Installing systemd service...")
        shutil.copy2(source_service, INSTALL_SERVICE)

        # Reload systemd
        subprocess.run(get_systemctl_cmd() + ["daemon-reload"], capture_output=True)
    else:
        log_warn("Systemd service file not found in source")

    # Fix ownership if running as root
    if os.environ.get("SUDO_USER"):
        uid = int(subprocess.run(["id", "-u", _real_user], capture_output=True, text=True).stdout.strip())
        gid = int(subprocess.run(["id", "-g", _real_user], capture_output=True, text=True).stdout.strip())

        os.chown(INSTALL_BINARY, uid, gid)
        os.chown(INSTALL_SERVICE, uid, gid)

        for f in INSTALL_CONFIG_DIR.iterdir():
            os.chown(f, uid, gid)

    print()
    log_info("SUCCESS. Installation complete.")
    log_info(f"Binary: {INSTALL_BINARY}")

    # Offer to enable service
    if not args.no_service and INSTALL_SERVICE.exists():
        if not is_service_enabled():
            print()
            response = input("Enable abraxas service? [Y/N]: ").strip().lower()
            if response in ("", "y", "yes"):
                enable_service()
        else:
            # Restart if already running
            restart_service()

    return True


def cmd_uninstall(args, source_dir: Path) -> bool:
    """Remove installed files."""
    log_info("UNINSTALLING ABRAXAS")

    # Disable service first
    if is_service_enabled():
        disable_service()

    files = [
        INSTALL_BINARY,
        INSTALL_SERVICE,
    ]

    removed = False
    for f in files:
        if f.exists():
            log_info(f"Removing {f}")
            f.unlink()
            removed = True

    if not removed:
        log_warn("No installed files found")
    else:
        log_info("Uninstall complete")

    # Note: Leave config directory alone (user data)
    if INSTALL_CONFIG_DIR.exists():
        log_info(f"Config directory preserved: {INSTALL_CONFIG_DIR}")

    return True


def cmd_status(args, source_dir: Path) -> bool:
    """Show installation status."""
    log_info("ABRAXAS STATUS")
    print()

    # Check installation
    binary_ok = INSTALL_BINARY.exists()
    service_ok = INSTALL_SERVICE.exists()
    config_ok = INSTALL_CONFIG_DIR.exists()
    zipdb_ok = (INSTALL_CONFIG_DIR / "us_zipcodes.bin").exists()

    print(f"  Binary:    {INSTALL_BINARY}")
    print(f"             {'installed' if binary_ok else 'NOT INSTALLED'}")
    print()
    print(f"  Service:   {INSTALL_SERVICE}")
    print(f"             {'installed' if service_ok else 'NOT INSTALLED'}")
    print()
    print(f"  Config:    {INSTALL_CONFIG_DIR}")
    print(f"             {'exists' if config_ok else 'NOT FOUND'}")
    print()
    print(f"  ZIP DB:    {INSTALL_CONFIG_DIR / 'us_zipcodes.bin'}")
    print(f"             {'installed' if zipdb_ok else 'NOT INSTALLED'}")
    print()

    # Service status
    if service_ok:
        enabled = is_service_enabled()
        active = is_service_active()
        print(f"  Service enabled: {'yes' if enabled else 'no'}")
        print(f"  Service running: {'yes' if active else 'no'}")
        print()

    # Check X11 libs
    x11_ok = check_x11_libs()
    print(f"  X11 fallback: {'available' if x11_ok else 'not available (install libx11 libxrandr)'}")
    print()

    print(f"  Overall: {'INSTALLED' if binary_ok else 'NOT INSTALLED'}")

    return binary_ok


def cmd_enable(args, source_dir: Path) -> bool:
    """Enable systemd service."""
    if not INSTALL_SERVICE.exists():
        log_error("Service file not installed. Run install first.")
        return False

    return enable_service()


def cmd_disable(args, source_dir: Path) -> bool:
    """Disable systemd service."""
    return disable_service()


def cmd_update(args, source_dir: Path) -> bool:
    """Update abraxas if source is newer."""
    log_info("CHECKING FOR UPDATES")

    needs_rebuild = False

    # Check if any source files are newer than installed binary
    if INSTALL_BINARY.exists():
        installed_mtime = INSTALL_BINARY.stat().st_mtime

        # Check daemon sources
        for pattern in ["src/*.[ch]", "include/*.[ch]", "libmeridian/src/*.[ch]",
                        "libmeridian/include/*.[ch]"]:
            for src_file in source_dir.glob(pattern):
                if src_file.stat().st_mtime > installed_mtime:
                    needs_rebuild = True
                    log_info(f"Source updated: {src_file.name}")
                    break
            if needs_rebuild:
                break

        if not needs_rebuild:
            log_info("Already up to date")
            return True
    else:
        needs_rebuild = True
        log_info("Binary: not installed")

    # Rebuild
    non_usa = getattr(args, 'non_usa', False)
    if not build_abraxas(source_dir, force=True, non_usa=non_usa):
        return False

    source_binary = source_dir / "abraxas"
    log_info("Installing updates...")
    shutil.copy2(source_binary, INSTALL_BINARY)
    INSTALL_BINARY.chmod(0o755)

    # Fix ownership if running as root
    if os.environ.get("SUDO_USER"):
        uid = int(subprocess.run(["id", "-u", _real_user], capture_output=True, text=True).stdout.strip())
        gid = int(subprocess.run(["id", "-g", _real_user], capture_output=True, text=True).stdout.strip())
        os.chown(INSTALL_BINARY, uid, gid)

    restart_service()
    log_info("Update complete")

    return True


# =============================================================================
# MAIN
# =============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install ABRAXAS dynamic color temperature daemon",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Commands:
  (default)   Install abraxas
  uninstall   Remove installed files
  status      Show installation status
  enable      Enable systemd service
  disable     Disable systemd service
  update      Update if source is newer

Examples:
  ./install.py                    # Install and optionally enable service
  ./install.py --no-service       # Install without prompting for service
  ./install.py --non-usa          # Install without NOAA weather (no libcurl)
  ./install.py status             # Check installation status
  ./install.py enable             # Enable service
  ./install.py uninstall          # Remove installation
"""
    )

    parser.add_argument("command", nargs="?", default="install",
                       choices=["install", "uninstall", "status", "enable", "disable", "update"],
                       help="Command to run (default: install)")
    parser.add_argument("--no-service", action="store_true",
                       help="Don't prompt to enable service after install")
    parser.add_argument("--rebuild", action="store_true",
                       help="Force rebuild even if already built")
    parser.add_argument("--non-usa", action="store_true",
                       help="Disable NOAA weather (no libcurl dependency)")

    args = parser.parse_args()

    source_dir = Path(__file__).parent.resolve()

    print()
    log_info("abraxas installer")
    log_info(f"Source: {source_dir}")
    print()

    # Run command
    commands = {
        "install": cmd_install,
        "uninstall": cmd_uninstall,
        "status": cmd_status,
        "enable": cmd_enable,
        "disable": cmd_disable,
        "update": cmd_update,
    }

    success = commands[args.command](args, source_dir)
    return 0 if success else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        sys.exit(130)
