#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright 2023 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
# License, and you may not use this file except in compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

"""
Stress test for monitoring how memory tracker behaves when
there is lot of node creation and deletions compared
to RES memory usage.
"""

from argparse import Namespace as Args
import atexit
from dataclasses import dataclass
from functools import wraps
import logging
import multiprocessing
import time
from typing import Any, Callable, Dict, List, Optional

from common import OutputData, SessionCache, connection_argument_parser, execute_till_success, try_execute

log = logging.getLogger(__name__)
output_data = OutputData()


class Constants:
    CREATE_FUNCTION = "CREATE"


atexit.register(SessionCache.cleanup)

MEMORY_LIMIT = 2048


def parse_args() -> Args:
    """
    Parses user arguments

    :return: parsed arguments
    """
    parser = connection_argument_parser()
    parser.add_argument("--worker-count", type=int, default=5, help="Number of concurrent workers.")
    parser.add_argument(
        "--logging", default="INFO", choices=["INFO", "DEBUG", "WARNING", "ERROR"], help="Logging level"
    )
    parser.add_argument("--repetition-count", type=int, default=1000, help="Number of times to perform the action")

    return parser.parse_args()


# Global variables


args = parse_args()

# Difference between memory RES and memory tracker on
# Memgraph start.
# Due to various other things which are included in RES
# there is difference of ~30MBs initially.
initial_diff = 0


@dataclass
class Worker:
    """
    Class that performs a function defined in the `type` argument.

    Args:
    type - either `CREATE` or `DELETE`, signifying the function that's going to be performed
        by the worker
    id - worker id
    total_worker_cnt - total number of workers for reference
    repetition_count - number of times to perform the worker action
    sleep_sec - float for subsecond sleeping between two subsequent actions
    """

    type: str
    id: int
    total_worker_cnt: int
    repetition_count: int
    sleep_sec: float


def timed_function(name) -> Callable:
    """
    Times performed function
    """

    def actual_decorator(func) -> Callable:
        @wraps(func)
        def timed_wrapper(*args, **kwargs) -> Any:
            start_time = time.time()
            result = func(*args, **kwargs)
            end_time = time.time()
            output_data.add_measurement(name, end_time - start_time)
            return result

        return timed_wrapper

    return actual_decorator


@timed_function("cleanup_time")
def clean_database() -> None:
    session = SessionCache.argument_session(args)
    execute_till_success(session, "MATCH (n) DETACH DELETE n")


def create_indices() -> None:
    session = SessionCache.argument_session(args)
    execute_till_success(session, "CREATE INDEX ON :Node")


def get_tracker_data(session) -> Optional[float]:
    def parse_data(allocated: str) -> float:
        text = allocated.strip()
        if text.endswith("KiB"):
            return float(text[:-3].strip()) / 1024.0
        if text.endswith("MiB"):
            return float(text[:-3].strip())
        if text.endswith("GiB"):
            return float(text[:-3].strip()) * 1024.0
        if text.endswith("TiB"):
            return float(text[:-3].strip()) * 1024.0 * 1024.0
        if text.endswith("B"):
            return float(text[:-1].strip()) / (1024.0 * 1024.0)
        return float(text)

    def isolate_value(data: List[Dict[str, Any]], key: str) -> Optional[str]:
        for row in data:
            if row["storage info"] == key:
                return row["value"]
        return None

    try:
        data, _ = try_execute(session, "SHOW STORAGE INFO")
        memory_tracker_data = isolate_value(data, "memory_tracked")
        if memory_tracker_data is None:
            return None

        return parse_data(memory_tracker_data)

    except Exception as ex:
        log.info("Get storage info failed with error: %s", ex)
        return None


def run_writer(repetition_count: int, sleep_sec: float, worker_id: int) -> tuple:
    """
    This writer creates lot of nodes on each write.
    Also it checks that query failed if memory limit is tried to be broken

    Return:
        (reached_oom, completed_writes)
    """

    session = SessionCache.argument_session(args)

    def try_create(curr_rep: int) -> str:
        """
        Function tries to create until memory limit is reached.
        Returns "executed" on committed write, "oom_refused" on verified memory-limit refusal,
        and raises RuntimeError on any unexpected write failure.
        """
        try:
            try_execute(
                session,
                "FOREACH (i in range(1,10000) | CREATE (:Node {prop:'big string or something like that'}))",
            )
            return "executed"
        except Exception as ex:
            output = str(ex)
            memory_over_2048_mb = False
            memory_tracker_data_after_start = get_tracker_data(session)
            if memory_tracker_data_after_start is not None:
                memory_over_2048_mb = memory_tracker_data_after_start >= 2048
            log.info(
                "Exception in create, exception output: %s | Worker %s started iteration %s, memory over 2048MB: %s",
                output,
                worker_id,
                curr_rep,
                memory_over_2048_mb,
            )
            if "Memory limit exceeded!" in output:
                return "oom_refused"
            raise RuntimeError(f"Worker {worker_id} failed write at iteration {curr_rep}: {output}") from ex

    curr_repetition = 0
    completed_writes = 0

    while curr_repetition < repetition_count:
        log.info("Worker %s started iteration %s", worker_id, curr_repetition)

        outcome = try_create(curr_repetition)
        if outcome == "oom_refused":
            return (True, completed_writes)

        completed_writes += 1
        time.sleep(sleep_sec)
        log.info("Worker %s created chain in iteration %s", worker_id, curr_repetition)

        curr_repetition += 1
    return (False, completed_writes)


def execute_function(worker: Worker) -> tuple:
    """
    Executes the function based on the worker type
    """
    if worker.type == Constants.CREATE_FUNCTION:
        reached_oom, completed_writes = run_writer(worker.repetition_count, worker.sleep_sec, worker.id)
        log.info(
            "Worker %s (id=%s) finished: reached_oom=%s, completed_writes=%s",
            worker.type,
            worker.id,
            reached_oom,
            completed_writes,
        )
        return (worker, reached_oom, completed_writes)

    raise Exception("Worker function not recognized, raising exception!")


@timed_function("total_execution_time")
def execution_handler() -> list:
    clean_database()
    log.info("Database is clean.")

    create_indices()

    worker_count = args.worker_count
    rep_count = args.repetition_count

    workers = []
    for i in range(worker_count):
        workers.append(Worker(Constants.CREATE_FUNCTION, i, worker_count, rep_count, 0.1))

    outcomes = []
    with multiprocessing.Pool(processes=worker_count) as p:
        for worker, reached_oom, completed_writes in p.map(execute_function, workers):
            log.info(
                "Worker %s (id=%s) finished: reached_oom=%s, completed_writes=%s",
                worker.type,
                worker.id,
                reached_oom,
                completed_writes,
            )
            outcomes.append((worker, reached_oom, completed_writes))
    return outcomes


if __name__ == "__main__":
    logging.basicConfig(level=args.logging)

    execution_handler()
    if args.logging in ["DEBUG", "INFO"]:
        output_data.dump()
