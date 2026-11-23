import argparse
import re
import subprocess
import sys

from aggregate_build_tests import list_daily_release_packages

# Compile regex patterns
URL_PATTERN = re.compile(r"^(https?://[\w\-\.]+(?:/[\w\-\./?%&=]*)?)$")
DATE_PATTERN = re.compile(r"^(?P<year>\d{4})(?P<month>0[1-9]|1[0-2])(?P<day>0[1-9]|[12]\d|3[01])$")
DOCKER_PATTERN = re.compile(
    r"^([a-z0-9]+(?:[._\-][a-z0-9]+)*(?:/[a-z0-9]+(?:[._\-][a-z0-9]+)*)*):([A-Za-z0-9][A-Za-z0-9._\-]*)$"
)
VERSION_PATTERN = re.compile(r"^\d+\.\d+(?:\.\d+)?.*$")
S3_PRE_DATE_PATTERN = re.compile(r"^\s*PRE\s+(\d{8})/\s*$")


def classify_string(s: str) -> str:
    """
    Classify the string provided to see if it is a URL, docker repo tag, or date.

    - [latest] when blank or 'latest' (resolve newest published daily build)
    - [url] for URLs to files (starting with http:// or https://)
    - [date] for dates in the format yyyymmdd
    - [docker] for Docker image name:tag
    - [version] for version e.g. v3.2.0

    If no pattern matches, returns the original string unchanged.
    """
    stripped = s.strip()
    if not stripped or stripped == "latest":
        return "latest"
    if URL_PATTERN.match(stripped):
        return "url"
    if DATE_PATTERN.match(stripped):
        return "date"
    if DOCKER_PATTERN.match(stripped):
        return "docker"
    if VERSION_PATTERN.match(stripped):
        return "version"
    return stripped


def list_available_daily_dates(image_type: str, mock: bool = False) -> list[str]:
    """Return published daily-build dates (YYYYMMDD) from S3 sorted newest-first."""
    s3_uri = f"s3://deps.memgraph.io/daily-build/{image_type}{'_mock' if mock else ''}/"
    proc = subprocess.run(
        ["aws", "s3", "ls", s3_uri],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or f"exit status {proc.returncode}"
        raise RuntimeError(f"Failed to list daily-build dates at {s3_uri}: {detail}")
    dates: list[str] = []
    for line in proc.stdout.splitlines():
        match = S3_PRE_DATE_PATTERN.match(line)
        if match:
            candidate = match.group(1)
            if DATE_PATTERN.match(candidate):
                dates.append(candidate)
    return sorted(set(dates), reverse=True)


def lookup_package_url(packages: dict, arch: str, malloc: bool, cuda: bool, relwithdebinfo: bool, image_type: str) -> str:
    """Extract the matching Docker image tarball URL from a daily package dictionary, or '' if absent."""
    arch_name = "x86_64" if arch == "amd" else "arm64"
    if image_type == "mage":
        key = f"Docker ({arch_name})"
    elif image_type == "memgraph":
        key = "docker"
    else:
        raise ValueError(f"Invalid image type: {image_type}")
    suffixes = ""
    if relwithdebinfo:
        suffixes += "-relwithdebinfo"
    if malloc:
        suffixes += "-malloc"
    if cuda:
        suffixes += "-cuda"
    key_image = f"{arch_name}{suffixes}"
    bucket_map = packages.get(key, {})
    if not isinstance(bucket_map, dict):
        return ""
    return str(bucket_map.get(key_image, ""))


def get_daily_url(date: str, arch: str, malloc: bool, cuda: bool, relwithdebinfo: bool, image_type: str) -> str:
    """
    Given a date of the format yyyymmdd, find and return the URL of the
    appropriate image, raising LookupError if no matching artifact was published.
    """
    packages = list_daily_release_packages(int(date), return_url=True, image_type=image_type)
    url = lookup_package_url(packages, arch, malloc, cuda, relwithdebinfo, image_type)
    if not url:
        raise LookupError(
            f"No {image_type} daily-build image found for date={date} arch={arch} "
            f"malloc={malloc} cuda={cuda} relwithdebinfo={relwithdebinfo}"
        )
    return url


def get_latest_daily_url(arch: str, malloc: bool, cuda: bool, relwithdebinfo: bool, image_type: str) -> str:
    """Walk published daily-build dates newest-first and return the newest matching image URL."""
    dates = list_available_daily_dates(image_type=image_type)
    for date in dates:
        packages = list_daily_release_packages(int(date), return_url=True, image_type=image_type)
        url = lookup_package_url(packages, arch, malloc, cuda, relwithdebinfo, image_type)
        if url:
            return url
    raise LookupError(
        f"No published {image_type} daily-build image found across {len(dates)} date(s) for "
        f"arch={arch} malloc={malloc} cuda={cuda} relwithdebinfo={relwithdebinfo}"
    )


def get_version_docker(version: str, malloc: str, cuda: str, relwithdebinfo: str, image_type: str):
    """
    convert version number to docker image tag

    This will only work for 3.0 onwards, for anything else supply the URL or
    full docker tag to the workflow
    """

    if image_type == "mage":
        repo_tag = f"memgraph/memgraph-mage:{version}"
    elif image_type == "memgraph":
        repo_tag = f"memgraph/memgraph:{version}"
    else:
        raise ValueError(f"Invalid image type: {image_type}")

    if relwithdebinfo and "relwithdebinfo" not in version:
        repo_tag = f"{repo_tag}-relwithdebinfo"
    if cuda and "cuda" not in version:
        repo_tag = f"{repo_tag}-cuda"
    if malloc and "malloc" not in version:
        repo_tag = f"{repo_tag}-malloc"

    return repo_tag


def string_to_boolean(bool_str: str) -> bool:
    """
    convert string to Boolean
    """

    return True if bool_str.lower() == "true" else False


def main() -> None:
    """
    This function will parse the inputs for the smoke test workflow and
    determine whether the input is a `date`, `URL`, `docker` tag or `version`.

    For...
    `date`:
        attempt to get the appropriate daily build, return URL
    `version`:
        get dockerhub repo:tag - not that this will only work for
        memgraph/MAGE > 3.0 (i.e. since their versions matched)
    `URL`:
        nothing needs to be done here if it's a full URL to an image archive
    `docker`:
        nothing needs to be done as long as it's a valid `repo:tag` string

    Finally, prints either:
        - "url https://...."
        - "docker memgraph/..."
    which is captured by BASH within the workflow to either download the archive
    or pull from dockerhub.
    """
    parser = argparse.ArgumentParser(description="Check image/url/date")

    parser.add_argument("image", type=str, help="Image tag, URL or daily build date")

    parser.add_argument("arch", type=str, help="CPU Arch: arm|amd")

    parser.add_argument("malloc", type=str, help="Is a malloc build: true|false")

    parser.add_argument("cuda", type=str, help="Is a CUDA build: true|false")

    parser.add_argument("relwithdebinfo", type=str, help="Is a RelWithDebInfo build: true|false")

    parser.add_argument("image_type", type=str, help="Image type: mage|memgraph")

    args = parser.parse_args()

    # classify image
    cls = classify_string(args.image)

    try:
        if cls == "latest":
            out = get_latest_daily_url(
                args.arch,
                string_to_boolean(args.malloc),
                string_to_boolean(args.cuda),
                string_to_boolean(args.relwithdebinfo),
                args.image_type,
            )
            cls = "url"
        elif cls == "date":
            out = get_daily_url(
                args.image,
                args.arch,
                string_to_boolean(args.malloc),
                string_to_boolean(args.cuda),
                string_to_boolean(args.relwithdebinfo),
                args.image_type,
            )
            cls = "url"
        elif cls == "version":
            out = get_version_docker(
                args.image,
                string_to_boolean(args.malloc),
                string_to_boolean(args.cuda),
                string_to_boolean(args.relwithdebinfo),
                args.image_type,
            )
            cls = "docker"
        elif cls in ("url", "docker"):
            out = args.image.strip()
        else:
            raise ValueError(f"Unrecognized image specifier: {args.image}")
    except (LookupError, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)

    print(f"{cls} {out}")


if __name__ == "__main__":
    main()
