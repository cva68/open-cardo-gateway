"""PlatformIO pre-build script: apply patches/framework-espidf/*.patch to the ESP-IDF package.

    [env:esp32dev]
    extra_scripts = pre:tools/apply_patches.py

Each patch is applied with `git apply` in the package directory
(~/.platformio/packages/framework-espidf). A patch that is already applied is skipped; one
that neither applies nor is already applied stops the build, e.g. after a framework update
changed the patched code. The package is shared by all PlatformIO projects using it.
"""

import subprocess
from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)

PATCH_DIR = Path(env.subst("$PROJECT_DIR")) / "patches" / "framework-espidf"  # noqa: F821
FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821


def git_apply(patch: Path, *args: str) -> subprocess.CompletedProcess:
    """Run `git apply [args] <patch>` in the framework directory."""
    return subprocess.run(["git", "apply", *args, str(patch)], cwd=FRAMEWORK_DIR,
                          capture_output=True, text=True)


for patch in sorted(PATCH_DIR.glob("*.patch")):
    if git_apply(patch, "--reverse", "--check").returncode == 0:
        continue  # already applied
    check = git_apply(patch, "--check")
    if check.returncode != 0:
        print(f"Patch {patch.name} doesn't apply to {FRAMEWORK_DIR}:\n{check.stderr}")
        env.Exit(1)  # noqa: F821
    git_apply(patch)
    print(f"Applied {patch.name} to {FRAMEWORK_DIR}")
