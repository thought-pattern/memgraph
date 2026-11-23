import argparse
import json
import os
import sys

# Single source of truth for the OS/arch (+ flavour) combinations MAGE can be
# packaged for.
#
# IMPORTANT: MAGE's python query-module dependencies (torch/PyG/DGL wheels) are
# currently cp312-only, so MAGE only works on distros whose embedded Python is
# 3.12 — ubuntu-24.04, centos-9 (built explicitly against python3.12), and
# centos-10 (ships 3.12 by default). The other distros Memgraph builds on
# (ubuntu-22.04 → 3.10, debian-12 → 3.11, debian-13/fedora-42 → 3.13) are
# commented out below; re-enabling them would require MAGE to support those
# Python versions (i.e. additional cp310/cp311/cp313 wheel builds).
#
# `label` is the PR-label suffix: apply `CI -package=mage-<label>` to a PR to
# trigger that build. ubuntu-24.04 is the primary target, so its amd/arm builds
# keep the short "amd"/"arm" labels and the cuda/cugraph/malloc flavour builds
# (all ubuntu-24.04 amd) keep their flavour labels; every other distro uses an
# "<os>"/"<os>-arm" label.
#
# Boolean-flavoured fields (cuda/cugraph/malloc) are emitted as JSON and consumed
# by package_mage.yaml via `${{ matrix.X == 'true' }}` comparisons. GitHub
# Actions coerces operands to numbers when comparing across types, so a Python
# bool `True` round-tripped through json.dumps as JSON `true` does NOT equal the
# string `'true'`. Keep them as strings here so the workflow comparisons resolve.
SUPPORTED_BUILDS = [
    {"label": "amd", "os": "ubuntu-24.04", "arch": "amd", "cuda": "false", "cugraph": "false", "malloc": "false"},
    {"label": "arm", "os": "ubuntu-24.04", "arch": "arm", "cuda": "false", "cugraph": "false", "malloc": "false"},
    {"label": "cuda", "os": "ubuntu-24.04", "arch": "amd", "cuda": "true", "cugraph": "false", "malloc": "false"},
    {"label": "cugraph", "os": "ubuntu-24.04", "arch": "amd", "cuda": "false", "cugraph": "true", "malloc": "false"},
    {"label": "malloc", "os": "ubuntu-24.04", "arch": "amd", "cuda": "false", "cugraph": "false", "malloc": "true"},
    # TODO(matt): validate MAGE on other Python versions - 3.14 would be particularly desirable for switching
    # to Ubuntu 26.04 as our Docker image base distro.
]

MATRIX_BUILD_LABELS = {"amd", "arm", "cuda", "cugraph", "malloc"}
MATRIX_BUILDS = [
    {k: v for k, v in build.items() if k != "label"}
    for build in SUPPORTED_BUILDS
    if build["label"] in MATRIX_BUILD_LABELS
]


def _build_docker_image(distro: str, cugraph: str, requested: str = "default") -> str:
    # An explicit build_docker_image dispatch input (none|prod|debug|both) wins.
    # "default" applies the heuristic: builds produce a debug MAGE image so the
    # docker smoke + e2e tests have something to run against. Only ubuntu-24.04
    # publishes a MAGE image; rpm distros build none (smoke-tested via the rpm
    # smoke image) and cugraph can't be smoke/e2e-tested in CI (GPU), so it
    # gets no image either.
    if requested and requested != "default":
        return requested
    return "debug" if (distro == "ubuntu-24.04" and cugraph != "true") else "none"


class PackageMageSetup:
    def __init__(self, gh_context_path: str, workflow_inputs_json: str = ""):
        self._gh_context_path = gh_context_path
        self._workflow_inputs_override = self._parse_inputs_json(workflow_inputs_json)
        self._package_suite = []
        self._load_gh_context()
        self.event_name = self._get_event_name()
        self.workflow_inputs = self.get_workflow_inputs()
        self.setup_package_workflow()

    @staticmethod
    def _parse_inputs_json(raw_json: str) -> dict:
        source = raw_json or os.environ.get("WORKFLOW_INPUTS_JSON", "")
        if not source or not source.strip():
            return {}
        loaded = json.loads(source)
        if not isinstance(loaded, dict):
            return {}
        normalized = {}
        for key, val in loaded.items():
            if val is True:
                normalized[key] = "true"
            elif val is False:
                normalized[key] = "false"
            elif val is not None:
                normalized[key] = str(val)
        return normalized

    def _load_gh_context(self) -> None:
        try:
            with open(self._gh_context_path, "r") as gh_context_file:
                self._gh_context = json.load(gh_context_file)
                if not self._get_event_name():
                    raise KeyError
        except FileNotFoundError:
            print(f"Error: file not found {self._gh_context_path}")
            sys.exit(1)
        except json.JSONDecodeError:
            print(f"Error: invalid JSON file {self._gh_context_path}")
            sys.exit(1)
        except KeyError:
            print(f"Error: invalid GitHub context file {self._gh_context_path}")
            sys.exit(1)

    def _get_event_name(self) -> str:
        return self._gh_context.get("event_name")

    def _get_pr_labels(self) -> list:
        return [label.get("name") for label in self._gh_context.get("event").get("pull_request").get("labels")]

    def get_package_suite(self) -> dict:
        return self._package_suite

    def _check_pr_label(self, build: dict, pr_labels: list) -> dict | None:
        default_args = {
            "memgraph_download_link": "",
            "memgraph_rpm_download_link": "",
            "memgraph_debuginfo_download_link": "",
            "push_to_s3": "false",
            "push_to_github": "false",
            "s3_dest_dir": "mage-unofficial",
            "run_smoke_tests": "true",
            "run_tests": "true",
            "package_mage": "default",
            "generate_sbom": "false",
            "ref": "",
        }
        label = build["label"]
        if f"CI -package=mage-{label}" in pr_labels:
            print(f'Found label for "{label}"')
            out = {
                "arch": build["arch"],
                "cuda": build["cuda"],
                "cugraph": build["cugraph"],
                "malloc": build["malloc"],
                "os": build["os"],
                "build_docker_image": _build_docker_image(build["os"], build["cugraph"]),
            }
            out.update(default_args)
            return out
        return None

    def _setup_pull_request(self) -> None:
        pr_labels = self._get_pr_labels()
        for build in SUPPORTED_BUILDS:
            result = self._check_pr_label(build, pr_labels)
            if result:
                self._package_suite.append(result)

    def get_workflow_inputs(self) -> dict:
        if self._get_event_name() in ["workflow_dispatch", "workflow_call"] or self._workflow_inputs_override:
            event_inputs = (self._gh_context.get("event") or {}).get("inputs") or {}
            merged = {}
            for key, val in event_inputs.items():
                if val is True:
                    merged[key] = "true"
                elif val is False:
                    merged[key] = "false"
                elif val is not None:
                    merged[key] = str(val)
            merged.update(self._workflow_inputs_override)
            return merged
        return {}

    def _check_workflow_input(self) -> list:
        # Inputs that apply uniformly to every build in the suite (i.e. not the
        # per-build arch/os/flavour that define a matrix entry).
        # GitHub passes workflow_dispatch/workflow_call inputs as strings
        # ("true"/"false"); keep the fallbacks as strings too so the matrix
        # always serialises booleans consistently — see MATRIX_BUILDS comment.
        common = {
            "push_to_s3": self.workflow_inputs.get("push_to_s3", "false"),
            "push_to_github": self.workflow_inputs.get("push_to_github", "false"),
            "s3_dest_dir": self.workflow_inputs.get("s3_dest_dir", "mage-unofficial"),
            "run_smoke_tests": self.workflow_inputs.get("run_smoke_tests", "false"),
            "run_tests": self.workflow_inputs.get("run_tests", "false"),
            "package_mage": self.workflow_inputs.get("package_mage", "default"),
            "ref": self.workflow_inputs.get("ref", ""),
        }
        requested_image = self.workflow_inputs.get("build_docker_image", "default") or "default"
        if requested_image not in ("default", "none", "prod", "debug", "both"):
            print(f"Error: invalid build_docker_image input '{requested_image}'")
            sys.exit(1)
        requested_sbom = self.workflow_inputs.get("generate_sbom", "false")
        if self.workflow_inputs.get("matrix_build") == "true":
            if requested_sbom == "true" and requested_image == "none":
                print("Error: generate_sbom=true requires build_docker_image != 'none'", file=sys.stderr)
                sys.exit(1)
            # arch/os/cuda/cugraph/malloc come from each MATRIX_BUILDS entry; the
            # remaining inputs above are applied uniformly across the matrix.
            suite = []
            for build in MATRIX_BUILDS:
                resolved_image = _build_docker_image(build["os"], build["cugraph"], requested_image)
                suite.append(
                    {
                        **build,
                        **common,
                        "build_docker_image": resolved_image,
                        "generate_sbom": "true" if (requested_sbom == "true" and resolved_image != "none") else "false",
                    }
                )
            return suite
        distro = self.workflow_inputs.get("os", "ubuntu-24.04")
        cugraph = self.workflow_inputs.get("cugraph", "false")
        resolved_image = _build_docker_image(distro, cugraph, requested_image)
        if requested_sbom == "true" and resolved_image == "none":
            print("Error: generate_sbom=true requires build_docker_image != 'none'", file=sys.stderr)
            sys.exit(1)
        return [
            {
                "arch": self.workflow_inputs.get("build_arch", "amd"),
                "cuda": self.workflow_inputs.get("cuda", "false"),
                "cugraph": cugraph,
                "malloc": self.workflow_inputs.get("malloc", "false"),
                "os": distro,
                "build_docker_image": resolved_image,
                "memgraph_download_link": self.workflow_inputs.get("memgraph_download_link", ""),
                "memgraph_rpm_download_link": self.workflow_inputs.get("memgraph_rpm_download_link", ""),
                "memgraph_debuginfo_download_link": self.workflow_inputs.get("memgraph_debuginfo_download_link", ""),
                "generate_sbom": requested_sbom,
                **common,
            }
        ]

    def setup_package_workflow(self) -> None:
        print(f"Event name: {self.event_name}")
        if self.event_name == "pull_request" and not self._workflow_inputs_override:
            self._setup_pull_request()
        elif self.event_name in ["workflow_dispatch", "workflow_call"] or self._workflow_inputs_override:
            self._package_suite = self._check_workflow_input()
        else:
            print("Invalid event name")
            sys.exit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Setup package workflow package suite")
    parser.add_argument(
        "--gh-context-path",
        type=str,
        required=True,
        help="Path to json file containing the GitHub context for workflow run",
    )
    parser.add_argument(
        "--workflow-inputs-json",
        type=str,
        default="",
        help="JSON-encoded workflow_call / workflow_dispatch `inputs` context",
    )
    return parser.parse_args()


def set_output(name: str, value: str) -> None:
    out_path = os.environ.get("GITHUB_OUTPUT")
    if not out_path:
        raise RuntimeError("GITHUB_OUTPUT is not set")
    with open(out_path, "a", encoding="utf-8") as f:
        f.write(f"{name}={value}\n")


def print_package_suite(package_suite: dict) -> None:
    for build in package_suite:
        print("--------------------------------")
        print(f"Build: {build}")
        print(f"Arch: {build.get('arch')}")
        print(f"CUDA: {build.get('cuda')}")
        print(f"Malloc: {build.get('malloc')}")
        print(f"Memgraph download link: {build.get('memgraph_download_link')}")
        print(f"Push to S3: {build.get('push_to_s3')}")
        print(f"S3 dest dir: {build.get('s3_dest_dir')}")
        print(f"Run smoke tests: {build.get('run_smoke_tests')}")
        print(f"Run tests: {build.get('run_tests')}")
        print(f"Package MAGE: {build.get('package_mage')}")
        print(f"Generate SBOM: {build.get('generate_sbom')}")
        print(f"Ref: {build.get('ref')}")
        print(f"OS: {build.get('os')}")

    build_packages = "true" if len(package_suite) > 0 else "false"
    set_output("build_packages", build_packages)

    suite_json = json.dumps(package_suite, separators=(",", ":"))
    set_output("package_suite", suite_json)


if __name__ == "__main__":
    args = parse_args()
    package_mage_setup = PackageMageSetup(args.gh_context_path, args.workflow_inputs_json)
    package_suite = package_mage_setup.get_package_suite()
    print_package_suite(package_suite)
    print(json.dumps(package_suite, indent=2))
