import { Driver, Session } from "neo4j-driver";

import { EMPTY } from "rxjs"
import { catchError, finalize, mergeMap, concatWith } from "rxjs/operators"

const neo4j = require("neo4j-driver");

const driver: Driver = neo4j.driver("bolt://localhost:7687");

function isExpectedTimeoutError(err: any): boolean {
  if (err == null) {
    return false;
  }
  const code = String(err.code || "");
  const message = String(err.message || "");
  if (
    code === "ServiceUnavailable" ||
    code === "SessionExpired" ||
    /ECONN|ENOTFOUND|EPIPE|socket|connect/i.test(message)
  ) {
    return false;
  }
  return (
    (code.includes("TransientError") || code.includes("TransactionTimedOut") || code === "") &&
    /transaction timeout|timed out/i.test(message)
  );
}

async function setup() {
  const session: Session = driver.session();

  try {
    await session.run('MATCH (n) DETACH DELETE n');
    await session.run('UNWIND RANGE(1, 100) AS x CREATE ()');
  } finally {
    await session.close();
  }
}

setup()
  .then(() => {
    const rxSession = driver.rxSession({ defaultAccessMode: 'READ' });
    rxSession
      .beginTransaction({ timeout: 50 })
      .pipe(
        mergeMap(tx =>
          tx
            .run('MATCH (),(),(),() RETURN 42 AS thing;')
            .records()
            .pipe(
              catchError(err => { tx.rollback(); throw err; }),
              concatWith(EMPTY.pipe(finalize(() => tx.commit())))
            )
        ),
        finalize(() => { rxSession.close(); driver.close() })
      )
      .subscribe({
        next: record => { },
        complete: () => { console.info('complete'); process.exit(1); },
        error: err => {
          console.error('Error:', err?.message ?? err);
          if (!isExpectedTimeoutError(err)) {
            process.exit(1);
          }
        },
      });
  })
  .catch(err => {
    console.error('Setup failed:', err?.message ?? err);
    driver.close();
    process.exit(1);
  });
