import sys

from client import *
import pytest

query = "MATCH (n) CALL libmodule_test.node(n) YIELD node RETURN node;"


def test_concurrent_module_access(client):
    client.initialize_to_execute(query, 200)
    client.initialize_to_execute(query, 200)
    client.initialize_to_execute(query, 200)

    success = client.execute_queries()
    assert success


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-rA"]))
