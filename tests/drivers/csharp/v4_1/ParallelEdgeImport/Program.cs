using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using Neo4j.Driver;

public class ParallelEdgeImport {
  public static async Task Main(string[] args) {
    var nodeCount = 100;
    var expectedEdgeCount = nodeCount - 1;
    using (var driver = GraphDatabase.Driver(
               "bolt://localhost:7687", AuthTokens.None,
               (builder) => builder.WithEncryptionLevel(EncryptionLevel.None)
                                .WithMaxTransactionRetryTime(TimeSpan.FromSeconds(60)))) {
      ClearDatabase(driver);

      // Create root
      using (var session = driver.Session()) {
        session.ExecuteWrite(tx => {
          var result = tx.Run("MERGE (root:Root) RETURN ID(root);");
          return result.Single()[0].As<int>();
        });
      }

      var initialNodes = CountNodes(driver);
      Trace.Assert(initialNodes == 1, "Node count after root creation is not correct!.");
      if (initialNodes != 1) {
        throw new InvalidOperationException($"Expected 1 root node, got {initialNodes}.");
      }

      // Importing edges asynchronously
      var tasks = new List<Task>();
      for (int i = 0; i < expectedEdgeCount; i++) {
        tasks.Add(ImportEdgeAsync(driver));
      }

      await Task.WhenAll(tasks);

      var finalNodes = CountNodes(driver);
      var finalEdges = CountEdges(driver);
      Trace.Assert(finalNodes == nodeCount);
      Trace.Assert(finalEdges == expectedEdgeCount);
      if (finalNodes != nodeCount || finalEdges != expectedEdgeCount) {
        throw new InvalidOperationException(
            $"Expected {nodeCount} nodes and {expectedEdgeCount} edges, got {finalNodes} nodes and {finalEdges} edges.");
      }
    }

    Console.WriteLine("All ok!");
  }

  private static void ClearDatabase(IDriver driver) {
    using (var session = driver.Session()) session.Run("MATCH (n) DETACH DELETE n").Consume();
  }

  private static async Task ImportEdgeAsync(IDriver driver) {
    using (var session = driver.AsyncSession()) {
      await session.WriteTransactionAsync(async tx => {
        var reader = await tx.RunAsync(
            "MATCH (root:Root) CREATE (n:Node) CREATE (n)-[:TROUBLING_EDGE]->(root) RETURN ID(root)");
        await reader.FetchAsync();
        Console.WriteLine("Transaction got through!");
        return reader.Current[0];
      });
    }
  }

  private static int CountNodes(IDriver driver) {
    using (var session = driver.Session()) {
      var result = session.Run("MATCH (n) RETURN COUNT(*) AS cnt");
      return Convert.ToInt32(result.First()["cnt"]);
    }
  }

  private static int CountEdges(IDriver driver) {
    using (var session = driver.Session()) {
      var result = session.Run("MATCH (:Node)-[r:TROUBLING_EDGE]->(:Root) RETURN COUNT(r) AS cnt");
      return Convert.ToInt32(result.First()["cnt"]);
    }
  }
}
