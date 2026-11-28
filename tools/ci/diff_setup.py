#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys


class DiffSetup:
    def __init__(self, base_branch: str, gh_context_path: str):
        self._base_branch = base_branch
        self._gh_context_path = gh_context_path
        self._get_default_test_suite(False)
        self._load_gh_context()

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

    def _get_workflow_dispatch_inputs(self) -> dict:
        return self._gh_context.get("event").get("inputs")

    def get_test_suite(self) -> dict:
        return self._test_suite

    def _get_default_test_suite(self, value: bool) -> None:
        self._test_suite = {
            "community": {"core": value},
            "coverage": {"core": value, "clang_tidy": value},
            "debug": {"core": value, "integration": value},
            "jepsen": {"core": value},
            "release": {"core": value, "benchmark": value, "e2e": value, "stress": value},
            "mage": {"amd": False, "arm": value, "cuda": False},
            "mgcxx": {"unit": value},
        }

    def _check_diff_workflow(self) -> bool:
        proc = subprocess.run(
            ["git", "diff", "--name-only", self._base_branch],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            err_msg = proc.stderr.strip() or f"git diff exited with status {proc.returncode}"
            print(f"Error: git diff against '{self._base_branch}' failed: {err_msg}", file=sys.stderr)
            sys.exit(1)
        for file in proc.stdout.splitlines():
            if file.startswith("licenses/"):
                continue
            elif file.endswith(".md"):
                continue
            elif file.startswith(".clang-format"):
                continue
            elif file.startswith("CODEOWNERS"):
                continue
            elif file.startswith(".github/workflows/") and not file.startswith(".github/workflows/diff"):
                continue
            elif file.startswith("tools/ci/pr-conflicts/"):
                continue
            return True
        return False

    def _check_pr_label(self, build: str, test: str, pr_labels: list) -> bool:
        if f"CI -build={build} -test={test}" in pr_labels:
            return True
        return False

    def _setup_pull_request(self) -> None:
        pr_labels = self._get_pr_labels()
        print(f"PR labels: {pr_labels}")
        for build, tests in self._test_suite.items():
            for test in tests.keys():
                self._test_suite[build][test] = self._check_pr_label(build, test, pr_labels)

    def _check_workflow_input(self, build: str, test: str, workflow_dispatch_inputs: dict) -> bool:
        if workflow_dispatch_inputs.get(f"{build}_{test}") == "true":
            return True
        return False

    def _setup_worfklow_dispatch(self) -> None:
        workflow_dispatch_inputs = self._get_workflow_dispatch_inputs()
        print(f"Workflow dispatch inputs: {workflow_dispatch_inputs}")
        for build, tests in self._test_suite.items():
            for test in tests.keys():
                self._test_suite[build][test] = self._check_workflow_input(build, test, workflow_dispatch_inputs)

    def setup_diff_workflow(self) -> None:
        event_name = self._get_event_name()
        print(f"Event name: {event_name}")
        if event_name == "merge_group":
            self._get_default_test_suite(self._check_diff_workflow())
        elif event_name == "schedule":
            self._get_default_test_suite(True)
        elif event_name == "pull_request":
            if self._check_diff_workflow():
                self._setup_pull_request()
            else:
                self._get_default_test_suite(False)
        elif event_name == "workflow_dispatch":
            self._setup_worfklow_dispatch()
        else:
            print("Invalid event name")
            sys.exit(1)


def print_test_suite(tests: dict, set_env_vars: bool = False) -> None:
    gh_output_path = os.environ.get("GITHUB_OUTPUT", "")
    for build, suite_tests in tests.items():
        for test, run in suite_tests.items():
            line = f"run_{build}_{test}={run}"
            print(line)
            if set_env_vars and gh_output_path:
                with open(gh_output_path, "a", encoding="utf-8") as gh_out:
                    gh_out.write(line + "\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Setup diff workflow test suite")
    parser.add_argument(
        "--gh-context-path",
        type=str,
        required=True,
        help="Path to json file containing the GitHub context for workflow run",
    )
    parser.add_argument(
        "--base-branch", default="origin/main", type=str, help="Base branch to compare against (default: origin/main)"
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    diff_setup = DiffSetup(args.base_branch, args.gh_context_path)
    diff_setup.setup_diff_workflow()
    test_suite = diff_setup.get_test_suite()
    print_test_suite(tests=test_suite, set_env_vars=True)
