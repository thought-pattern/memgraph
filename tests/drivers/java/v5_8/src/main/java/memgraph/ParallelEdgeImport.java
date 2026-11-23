package memgraph;

import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.neo4j.driver.AuthTokens;
import org.neo4j.driver.Config;
import org.neo4j.driver.Driver;
import org.neo4j.driver.GraphDatabase;
import org.neo4j.driver.Session;
import org.neo4j.driver.Transaction;
import org.neo4j.driver.TransactionWork;

public class ParallelEdgeImport {
  private static final AtomicBoolean workerFailed = new AtomicBoolean(false);

  public static int dropDatabase(Driver driver) {
    try (Session session = driver.session()) {
      session.writeTransaction(new TransactionWork<Integer>() {
        @Override
        public Integer execute(Transaction tx) {
          tx.run("MATCH (n) DETACH DELETE n;").consume();
          return 0;
        }
      });
    }
    return 0;
  }

  public static int createRoot(Driver driver) {
    try (var session = driver.session()) {
      session.writeTransaction(new TransactionWork<Integer>() {
        @Override
        public Integer execute(Transaction tx) {
          tx.run("MERGE (n:Root) RETURN ID(n);").consume();
          return 0;
        }
      });
    }
    return 0;
  }

  public static void parallelEdgeImport(Driver d, int nodeCount) {
    try (var session = d.session()) {
      session.writeTransaction(new TransactionWork<Integer>() {
        @Override
        public Integer execute(Transaction tx) {
          tx.run("MATCH (root:Root) CREATE (n:Node) CREATE (n)-[:TROUBLING_EDGE]->(root);")
              .consume();
          System.out.println("Transaction got through!");
          return 0;
        }
      });
    } catch (Exception e) {
      System.out.println(e);
      workerFailed.set(true);
    }
  }

  private static Config config = Config.builder()
                        .withoutEncryption()
                        .withMaxTransactionRetryTime(15, TimeUnit.SECONDS)
                        .build();

  private static Driver driver = GraphDatabase.driver("bolt://localhost:7687", AuthTokens.none(), config);
  private static int nodeCount = 100;

  static class MyRunnable implements Runnable {
    @Override
    public void run() {
      parallelEdgeImport(driver, nodeCount);
    }
  }

  public static void main(String[] args) {
    int expectedEdgeCount = nodeCount - 1;
    dropDatabase(driver);
    System.out.println("Database cleared!");

    createRoot(driver);
    System.out.println("Root created!");

    List<Thread> threadList = new ArrayList<>();
    for (int i = 0; i < expectedEdgeCount; i++) {
      Runnable myRunnable = new MyRunnable();
      Thread thread = new Thread(myRunnable);
      threadList.add(thread);
      thread.start();
    }

    for (Thread thread : threadList) {
      try {
        thread.join();
      } catch (InterruptedException e) {
        e.printStackTrace();
        workerFailed.set(true);
      }
    }

    if (workerFailed.get()) {
      driver.close();
      System.exit(1);
    }

    try (var session = driver.session()) {
      var actualNodeCount = session.run("MATCH (n) RETURN COUNT(n) AS cnt;").list().get(0).get("cnt").asInt();
      if (actualNodeCount != nodeCount) {
        System.out.println("Node count doesn't match (" + actualNodeCount + ")");
        driver.close();
        System.exit(1);
      }
      var actualEdgeCount = session
          .run("MATCH (:Node)-[r:TROUBLING_EDGE]->(:Root) RETURN COUNT(r) AS cnt;")
          .list()
          .get(0)
          .get("cnt")
          .asInt();
      if (actualEdgeCount != expectedEdgeCount) {
        System.out.println("Edge count doesn't match (" + actualEdgeCount + ")");
        driver.close();
        System.exit(1);
      }
    }

    System.out.println("All ok!");
    driver.close();
  }
}
