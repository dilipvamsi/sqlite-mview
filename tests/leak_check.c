
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
** Simple test runner to check for memory leaks.
** Links against sqlite3 and loads mview extension.
*/

static void check_rc(int rc, const char *msg, sqlite3 *db) {
  if (rc != SQLITE_OK) {
    fprintf(stderr, "FAIL: %s - %s\n", msg, sqlite3_errmsg(db));
    exit(1);
  }
}

static void exec_sql(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, 0, 0, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL Error: %s\nQuery: %s\n", err, sql);
    sqlite3_free(err);
    exit(1);
  }
}

int main() {
  sqlite3 *db;
  int rc;

  printf("Running Leak Check...\n");

  // 1. Open Memory DB
  rc = sqlite3_open(":memory:", &db);
  check_rc(rc, "Open DB", db);

  // 2. Load Extension
  sqlite3_enable_load_extension(db, 1);
  rc = sqlite3_load_extension(db, "./mview", 0, 0);
  check_rc(rc, "Load Extension", db);

  // 3. Setup Data
  exec_sql(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
  exec_sql(db, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');");

  // 4. Attach & Init
  // Note: In memory DB, we can attach a separate file or just init?
  // mview_attach expects a path. Let's use a temp file.
  const char *cache_path = getenv("TEST_DB_PATH");
  if (!cache_path)
    cache_path = "leak_test_cache.db";

  // Attempt clean first
  remove(cache_path);
  char wal_path[256], shm_path[256];
  snprintf(wal_path, sizeof(wal_path), "%s-wal", cache_path);
  snprintf(shm_path, sizeof(shm_path), "%s-shm", cache_path);
  remove(wal_path);
  remove(shm_path);

  char buf[512];
  snprintf(buf, sizeof(buf), "SELECT mview_attach('%s')", cache_path);
  exec_sql(db, buf);
  exec_sql(db, "SELECT mview_init()");

  // 5. Run Operations (Full Cycle)

  // Create
  exec_sql(db, "SELECT mview_create('v1', 'SELECT * FROM users')");

  // List & Info
  exec_sql(db, "SELECT * FROM mview_registry");
  exec_sql(db, "SELECT mview_info('v1')");

  // Refresh
  exec_sql(db, "INSERT INTO users VALUES (3, 'Charlie')");
  exec_sql(db, "SELECT mview_refresh('v1')");

  // Stats & Verify
  exec_sql(db, "SELECT mview_stats('v1')");
  exec_sql(db, "SELECT mview_verify('v1')");

  // Indexing
  exec_sql(db, "SELECT mview_add_index('v1', 'name', 0)");
  exec_sql(db, "SELECT mview_refresh('v1')"); // Re-applies index
  exec_sql(db, "SELECT mview_remove_index('v1', 'name')");

  // Rename
  exec_sql(db, "SELECT mview_rename('v1', 'v_renamed')");

  // Export & Explain
  exec_sql(db, "SELECT mview_export('v_renamed')");
  exec_sql(db, "SELECT mview_explain('v_renamed')");

  // Bulk Ops
  exec_sql(db, "SELECT mview_create('v2', 'SELECT 1')");
  exec_sql(db, "SELECT mview_refresh_all()");

  // New introspection functions
  exec_sql(db, "SELECT mview_has('v_renamed')");
  exec_sql(db, "SELECT mview_has('nonexistent')");
  exec_sql(db, "SELECT mview_query('v_renamed')");
  exec_sql(db, "SELECT mview_schema('v_renamed')");
  exec_sql(db, "SELECT mview_indexes('v_renamed')");
  exec_sql(db, "SELECT mview_size('v_renamed')");
  exec_sql(db, "SELECT mview_count()");

  // Time-based functions
  exec_sql(db, "SELECT mview_last_refreshed('v_renamed')");
  exec_sql(db, "SELECT mview_age('v_renamed')");
  exec_sql(db, "SELECT mview_stale('v_renamed', 60)");
  exec_sql(db, "SELECT mview_refresh_stale(300)");

  // Truncate
  exec_sql(db, "SELECT mview_truncate('v_renamed')");

  // Logging functions
  exec_sql(db, "SELECT mview_log_enable(1)");
  exec_sql(db, "SELECT mview_refresh('v_renamed')"); // Trigger a log entry
  exec_sql(db, "SELECT mview_log()");
  exec_sql(db, "SELECT mview_log(10)");
  exec_sql(db, "SELECT mview_log_clear()");
  exec_sql(db, "SELECT mview_log_enable(0)");

  // Drop
  exec_sql(db, "SELECT mview_drop('v_renamed')");
  exec_sql(db, "SELECT mview_drop_all()");

  // Dettach
  exec_sql(db, "SELECT mview_dettach()");

  // 6. Close
  sqlite3_close(db);

  // Cleanup file
  remove(cache_path);
  remove(wal_path);
  remove(shm_path);

  printf("Done.\n");
  return 0;
}
