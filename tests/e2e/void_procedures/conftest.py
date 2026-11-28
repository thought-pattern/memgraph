from common import connect, execute_and_fetch_all
import pytest


@pytest.fixture(autouse=True)
def connection():
    connection = connect()
    yield connection
    cursor = connection.cursor()
    execute_and_fetch_all(cursor, "MATCH (n) DETACH DELETE n")
