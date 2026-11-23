// Determines how long could be a query executed
// from JavaScript driver.
//
// Performs binary search until the maximum possible
// query size has been found inside the search range.

var neo4j = require('neo4j-driver').v1;
var driver = neo4j.driver("bolt://localhost:7687",
                          neo4j.auth.basic("", ""),
                          { encrypted: 'ENCRYPTION_OFF' });

var min_len = 1;
var max_len = 1000000;
var best_query_len = -1;
var observed_failure = false;

function finish() {
  driver.close();
  if (best_query_len < 0) {
    console.error("\nNo query succeeded; all tested query lengths failed.\n");
    process.exit(1);
  }
  if (!observed_failure) {
    console.log(
      "\nAll tested queries succeeded up to search ceiling (>= " +
      best_query_len +
      " characters); actual maximum query length exceeds tested range.\n"
    );
    process.exit(0);
  }
  console.log("\nThe max length of a query from JS driver is: " + best_query_len + "\n");
  process.exit(0);
}

function step() {
  if (min_len > max_len) {
    finish();
    return;
  }
  var property_size = Math.floor((min_len + max_len) / 2);
  var query = "CREATE (n {name:\"" + (new Array(property_size + 1)).join("a") + "\"})";
  var session = driver.session();
  session.run(query, {}).then(function () {
    console.log("Success with the query length " + query.length);
    best_query_len = query.length;
    min_len = property_size + 1;
  }).catch(function () {
    console.log("Failure with the query length " + query.length);
    observed_failure = true;
    max_len = property_size - 1;
  }).then(function () {
    session.close();
    step();
  });
}

console.log("\nDetermine how long can be a query sent from JavaScript driver.");
step();
