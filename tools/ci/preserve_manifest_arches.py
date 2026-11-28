#!/usr/bin/env python3
"""Resolve `docker manifest create --amend` arguments while preserving existing remote architectures."""

from argparse import ArgumentParser
from json import JSONDecodeError, loads
from subprocess import run
from sys import argv, exit as sys_exit


def inspect_remote_arch_digests(image_ref: str) -> dict[str, str]:
    """Return existing linux arch -> digest entries from a remote manifest list or image manifest."""
    proc = run(
        ["docker", "manifest", "inspect", "-v", image_ref],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return {}
    raw = proc.stdout.strip()
    if not raw:
        return {}
    try:
        parsed = loads(raw)
    except JSONDecodeError:
        return {}

    digests: dict[str, str] = {}
    if isinstance(parsed, list):
        entries = parsed
    elif isinstance(parsed, dict) and isinstance(parsed.get("manifests", []), list) and parsed.get("manifests", []):
        entries = parsed.get("manifests", [])
    elif isinstance(parsed, dict):
        entries = [parsed]
    else:
        entries = []

    for entry in entries:
        if not isinstance(entry, dict):
            continue
        desc = entry.get("Descriptor", entry)
        if not isinstance(desc, dict):
            continue
        platform = desc.get("platform", {})
        if not isinstance(platform, dict):
            continue
        os_name = str(platform.get("os", "linux")).lower()
        if os_name and os_name != "linux":
            continue
        arch = str(platform.get("architecture", "")).lower()
        digest = str(desc.get("digest", "")).strip()
        if not digest.startswith("sha256:"):
            continue
        if arch == "amd64":
            digests["amd64"] = digest
        elif arch in ("arm64", "aarch64"):
            digests["arm64"] = digest
    return digests


def resolve_amend_refs(repo: str, tag: str, amd_ref: str, arm_ref: str, existing_digests: dict[str, str]) -> list[str]:
    """Return ordered `--amend <ref>` CLI tokens merging newly pushed arches with preserved remote arches."""
    chosen_amd = amd_ref.strip()
    if not chosen_amd and existing_digests.get("amd64", ""):
        chosen_amd = f"{repo}@{existing_digests.get('amd64', '')}"

    chosen_arm = arm_ref.strip()
    if not chosen_arm and existing_digests.get("arm64", ""):
        chosen_arm = f"{repo}@{existing_digests.get('arm64', '')}"

    args: list[str] = []
    if chosen_amd:
        args.extend(["--amend", chosen_amd])
    if chosen_arm:
        args.extend(["--amend", chosen_arm])
    if not args:
        raise ValueError(f"No amd64 or arm64 image reference available for {repo}:{tag}")
    return args


def main() -> int:
    """CLI entrypoint printing newline-delimited `--amend` arguments for `docker manifest create`."""
    parser = ArgumentParser(description="Preserve existing multi-arch manifest members during single-arch promotion")
    parser.add_argument("--repo", required=True, type=str)
    parser.add_argument("--tag", required=True, type=str)
    parser.add_argument("--amd-ref", default="", type=str)
    parser.add_argument("--arm-ref", default="", type=str)
    parsed = parser.parse_args(argv[1:])

    existing: dict[str, str] = {}
    if not parsed.amd_ref or not parsed.arm_ref:
        existing = inspect_remote_arch_digests(f"{parsed.repo}:{parsed.tag}")
    amend_tokens = resolve_amend_refs(parsed.repo, parsed.tag, parsed.amd_ref, parsed.arm_ref, existing)
    print(" ".join(amend_tokens))
    return 0


if __name__ == "__main__":
    sys_exit(main())
