/**
 * Determines how long could be a query executed
 * from Java driver.
 *
 * Performs binary search until the maximum possible
 * query size has been found inside the search range.
 */
package memgraph;

import static org.neo4j.driver.Values.parameters;

import java.util.*;
import org.neo4j.driver.*;
import org.neo4j.driver.types.*;

public class MaxQueryLength {
  public static void main(String[] args) {
    Config config = Config.builder().withoutEncryption().build();
    Driver driver = GraphDatabase.driver("bolt://localhost:7687", AuthTokens.basic("", ""), config);
    int min_len = 1;
    int max_len = 100000;
    int best_query_len = -1;
    boolean observed_failure = false;
    String query_template = "CREATE (n {name:\"%s\"})";
    int template_size = query_template.length() - 2; // because of %s

    try {
      while (min_len <= max_len) {
        int property_size = min_len + (max_len - min_len) / 2;
        try (Session session = driver.session()) {
          String property_value = new String(new char[property_size]).replace('\0', 'a');
          String query = String.format(query_template, property_value);
          session.run(query).consume();
          best_query_len = query.length();
          min_len = property_size + 1;
        } catch (Exception e) {
          System.out.println(
              String.format("Query length: %d; Error: %s", property_size + template_size, e));
          observed_failure = true;
          max_len = property_size - 1;
        }
      }
    } finally {
      driver.close();
    }

    if (best_query_len < 0) {
      System.err.println("\nNo query succeeded; all tested query lengths failed.\n");
      System.exit(1);
    }
    if (!observed_failure) {
      System.out.println(String.format(
          "\nAll tested queries succeeded up to search ceiling (>= %d characters); "
              + "actual maximum query length exceeds tested range.\n",
          best_query_len));
      return;
    }

    System.out.println(String.format(
        "\nThe max length of a query executed from Java driver is: %d\n",
        best_query_len));
  }
}
