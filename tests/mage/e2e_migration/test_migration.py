"""
Migration testing module for e2e_migration tests.
Tests the migration of data from various databases to Memgraph using the migrate module.
"""
import logging
from pathlib import Path
from typing import Any, Dict, List

from gqlalchemy import Memgraph
import pytest
import yaml

INFRASTRUCTURE_OR_SCHEMA_ERROR_MARKERS = (
    "connection refused",
    "could not connect",
    "failed to connect",
    "can't connect",
    "cannot connect",
    "connection reset",
    "server closed the connection",
    "connection timed out",
    "timeout expired",
    "timed out",
    "could not translate host name",
    "name or service not known",
    "temporary failure in name resolution",
    "network is unreachable",
    "no route to host",
    "authentication failed",
    "access denied",
    "session expired",
    "serviceunavailable",
    "does not exist",
    "undefinedcolumn",
    "undefinedtable",
    "unknown column",
    "there is no procedure named",
    "enterprise license",
)

EXPECTED_CONVERSION_OR_QUERY_ERROR_MARKERS = (
    "unsupported pyobject conversion",
    "database query parameter values",
    "already running",
    "enum type is not yet supported",
    "tostring",
    "valuetype",
    "argument",
    "invalid type",
    "must be",
    "cannot convert",
    "unsupported",
)


def is_expected_migration_failure(exc: Exception, test_config: Dict[str, Any]) -> bool:
    """Return True only when exc represents the expected conversion/query rejection rather than an outage."""
    if isinstance(exc, (ConnectionError, TimeoutError, OSError)):
        return False
    cls_name = type(exc).__name__.lower()
    if any(tok in cls_name for tok in ("connection", "serviceunavailable", "sessionexpired", "auth")):
        return False

    msg = str(exc)
    msg_lower = msg.lower()
    if any(marker in msg_lower for marker in INFRASTRUCTURE_OR_SCHEMA_ERROR_MARKERS):
        return False

    expected_spec = test_config.get("exception", False)
    if isinstance(expected_spec, str) and expected_spec.strip():
        return expected_spec.strip().lower() in msg_lower

    expected_contains = test_config.get("exception_contains")
    if isinstance(expected_contains, str) and expected_contains.strip():
        return expected_contains.strip().lower() in msg_lower

    return any(marker in msg_lower for marker in EXPECTED_CONVERSION_OR_QUERY_ERROR_MARKERS)


def load_test_config(test_file_path: str) -> Dict[str, Any]:
    """Load test configuration from YAML file."""
    try:
        with open(test_file_path, "r") as file:
            config = yaml.safe_load(file)
        return config
    except Exception as e:
        logger.error(f"Failed to load test config from {test_file_path}: {e}")
        raise


def prepare_migration_tests() -> List[pytest.param]:
    """
    Fetch all the migration tests and prepare them for execution.
    Returns a list of pytest.param objects with test directory and file information.
    """
    tests = []
    current_dir = Path(__file__).parent

    for test_dir in current_dir.iterdir():
        if not test_dir.is_dir() or not test_dir.name.startswith("test_"):
            continue

        test_path = test_dir / "test"
        if not test_path.exists():
            continue

        for subdir in test_path.iterdir():
            if not subdir.is_dir():
                continue

            for yml_file in subdir.glob("*.yml"):
                test_id = f"{test_dir.name}-{subdir.name}-{yml_file.stem}"
                tests.append(pytest.param(test_dir.name, str(yml_file.relative_to(test_dir)), id=test_id))

    return sorted(tests, key=lambda x: x.id)


migration_tests = prepare_migration_tests()


logging.basicConfig(format="%(asctime)-15s [%(levelname)s]: %(message)s")
logger = logging.getLogger("e2e_migration")
logger.setLevel(logging.INFO)


@pytest.fixture
def db():
    """Fixture to provide Memgraph database connection."""
    return Memgraph()


@pytest.mark.parametrize("test_dir,test_file", migration_tests)
def test_migration(test_dir: str, test_file: str, db: Memgraph):
    script_dir = Path(__file__).parent
    test_config_path = script_dir / test_dir / test_file
    test_config = load_test_config(str(test_config_path))

    expect_exception = bool(test_config.get("exception", False) or test_config.get("exception_contains"))

    try:
        migration_query = test_config["query"]
        memgraph_results = list(db.execute_and_fetch(migration_query))
    except Exception as e:
        if expect_exception and is_expected_migration_failure(e, test_config):
            logger.info(f"Migration failed as expected: {e}")
            return
        raise

    if expect_exception:
        pytest.fail("Expected migration to fail with an exception, but it succeeded")

    expected_results = test_config["output"]

    assert len(memgraph_results) == len(
        expected_results
    ), f"Result count mismatch: expected {len(expected_results)}, got {len(memgraph_results)}"

    for i, (actual_row, expected_row) in enumerate(zip(memgraph_results, expected_results)):
        assert isinstance(actual_row, dict), f"Expected dict row at index {i}, got {type(actual_row).__name__}"
        missing_fields = [field for field in expected_row if field not in actual_row]
        assert not missing_fields, (
            f"Missing expected column(s) {missing_fields} at row {i}: got columns {list(actual_row.keys())}"
        )
        extra_fields = [field for field in actual_row if field not in expected_row]
        assert not extra_fields, (
            f"Unexpected extra column(s) {extra_fields} at row {i}: expected columns {list(expected_row.keys())}"
        )
        for field, expected_value in expected_row.items():
            actual_value = actual_row[field]
            assert (
                isinstance(actual_value, bool) == isinstance(expected_value, bool) and actual_value == expected_value
            ), f"Field {field} mismatch at row {i}: expected {expected_value!r}, got {actual_value!r}"

    logger.info("Data migration to Memgraph successfully validated!")
