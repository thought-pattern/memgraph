import static org.neo4j.driver.Values.parameters;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.neo4j.driver.AuthTokens;
import org.neo4j.driver.Config;
import org.neo4j.driver.Driver;
import org.neo4j.driver.GraphDatabase;
import org.neo4j.driver.Record;
import org.neo4j.driver.Result;
import org.neo4j.driver.Session;
import org.neo4j.driver.Transaction;
import org.neo4j.driver.TransactionConfig;
import org.neo4j.driver.Value;

public class Metadata {
  public static String createPerson(Transaction tx, String name) {
    Result result =
        tx.run("CREATE (a:Person {name: $name}) RETURN a.name", parameters("name", name));
    return result.single().get(0).asString();
  }

  public static void checkMd(
      Result result, String expectedVer, String expectedStr, int expectedNum) {
    int n = 0;
    while (result.hasNext()) {
      Record r = result.next();
      Value md = r.get("metadata");
      if (md != null && !md.isNull()
          && !md.get("ver").isNull()
          && !md.get("str").isNull()
          && !md.get("num").isNull()
          && Objects.equals(md.get("ver").asString(), expectedVer)
          && Objects.equals(md.get("str").asString(), expectedStr)
          && Objects.equals(md.get("num").asInt(), expectedNum)) {
        n = n + 1;
      }
    }
    if (n == 0) {
      System.out.println("Error while reading metadata!");
      System.exit(1);
    }
  }

  public static void main(String[] args) {
    Config config = Config.builder()
                        .withoutEncryption()
                        .withMaxTransactionRetryTime(0, TimeUnit.SECONDS)
                        .build();
    Driver driver =
        GraphDatabase.driver("bolt://localhost:7687", AuthTokens.basic("neo4j", "1234"), config);

    try (Session session = driver.session()) {
      // Explicit transaction
      System.out.println("Checking explicit transaction metadata...");
      TransactionConfig explicitConfig =
          TransactionConfig.builder()
              .withTimeout(Duration.ofSeconds(2))
              .withMetadata(Map.ofEntries(Map.entry("ver", "transaction"),
                  Map.entry("str", "oho"), Map.entry("num", 456)))
              .build();
      try (Transaction tx = session.beginTransaction(explicitConfig)) {
        tx.run("MATCH (n) RETURN n LIMIT 1").consume();
        AtomicBoolean threadFailed = new AtomicBoolean(false);
        Thread thread = new Thread(() -> {
          try (Session s = driver.session()) {
            checkMd(s.run("SHOW TRANSACTIONS"), "transaction", "oho", 456);
          } catch (Exception e) {
            System.out.println(e);
            threadFailed.set(true);
          }
        });
        thread.start();
        thread.join();
        if (threadFailed.get()) {
          System.exit(1);
        }
        tx.commit();
      }

      // Implicit transaction
      System.out.println("Checking implicit transaction metadata...");
      TransactionConfig implicitConfig =
          TransactionConfig.builder()
              .withTimeout(Duration.ofSeconds(2))
              .withMetadata(Map.ofEntries(Map.entry("ver", "session"),
                  Map.entry("str", "aha"), Map.entry("num", 123)))
              .build();
      checkMd(session.run("SHOW TRANSACTIONS", implicitConfig), "session", "aha", 123);

      System.out.println("All ok!");
    } catch (Exception e) {
      System.out.println(e);
      driver.close();
      System.exit(1);
    }

    driver.close();
  }
}
