/*
** ============================================================================
** SQLite Memorized Views (sqlite-mview)
** ============================================================================
**
** DESCRIPTION:
**   This extension implements "Materialized Views" (Memorized Queries) for
* SQLite.
**   Standard SQLite views are virtual; they re-run the query every time you
* read them.
**   This extension stores the results of complex queries in real physical
* tables
**   inside a separate "Attached Database".
**
**   Benefits:
**   1. Performance: Reads are instant (O(1) or O(log n) with indexes).
**   2. Isolation:   View data lives in a separate file, keeping the main DB
* clean.
**   3. Atomicity:   Refreshes happen in transactions (no partial data).
**
** ============================================================================
** COMPILATION INSTRUCTIONS
** ============================================================================
**
** LINUX / MACOS:
**   gcc -g -fPIC -shared mview_extension.c -o mview.so
**
** WINDOWS (MinGW):
**   gcc -g -shared mview_extension.c -o mview.dll
**
** ============================================================================
** USAGE EXAMPLES (SQL)
** ============================================================================
**
** 1. LOAD EXTENSION:
**    .load ./mview
**
** 2. INITIALIZE (Attach the cache database):
**    -- This creates 'cache.db' if missing and attaches it as 'mviews'
**    SELECT mview_init('cache.db');
**
** 3. CREATE A VIEW (Simple Mode - Auto Types):
**    -- Creates table 'mviews.daily_sales'
**    SELECT mview_create('daily_sales',
**        'SELECT date, sum(total) FROM main.orders GROUP BY date'
**    );
**
** 4. CREATE A VIEW (Strict Mode - Custom Schema):
**    -- Useful for adding Primary Keys for performance
**    SELECT mview_create('users_mv',
**        'SELECT id, username FROM main.users',
**        'id INTEGER PRIMARY KEY, username TEXT'
**    );
**
** 5. QUERY DATA:
**    SELECT * FROM mviews.daily_sales WHERE total > 1000;
**
** 6. REFRESH DATA:
**    -- Re-runs the query and updates the table atomically
**    SELECT mview_refresh('daily_sales');
**
** 7. DROP VIEW:
**    SELECT mview_drop('daily_sales');
**
** 8. CLOSE / DETACH:
**    SELECT mview_close();
**
** ============================================================================
*/

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <string.h>

/*
** CONSTANT: CACHE_SCHEMA
** We force the attached database to have this specific alias ("mviews").
** This ensures our SQL statements (like 'INSERT INTO mviews.table...')
** always target the correct database.
*/
#define CACHE_SCHEMA "mviews"

/*
** HELPER: quote_identifier
** ----------------------------------------------------------------------------
** Wraps a string in double quotes and escapes internal double quotes.
** Example:  my table  ->  "my table"
** Example:  o'neil    ->  "o'neil"
** Example:  bad"name  ->  "bad""name"
**
** WHY THIS IS NEEDED:
** The standard %q in sqlite3_mprintf escapes strings for single quotes
* (values).
** The %w format escapes identifiers (tables/columns), but is only available
** in SQLite 3.28+. To ensure this extension works on older systems, we
** implement this manually.
**
** RETURNS: A new string (Must be freed with sqlite3_free).
*/
static char *quote_identifier(const char *in) {
  if (!in)
    return sqlite3_mprintf("\"\"");

  // Count quotes to allocate correct size
  int len = 0;
  int extra = 0;
  for (const char *p = in; *p; p++) {
    len++;
    if (*p == '"')
      extra++;
  }

  // Allocate: len + extra(escapes) + 2(wrapping quotes) + 1(null terminator)
  char *out = sqlite3_malloc(len + extra + 3);
  if (!out)
    return NULL;

  char *p = out;
  *p++ = '"'; // Start quote
  for (int i = 0; i < len; i++) {
    if (in[i] == '"') {
      *p++ = '"'; // Escape " as ""
      *p++ = '"';
    } else {
      *p++ = in[i];
    }
  }
  *p++ = '"'; // End quote
  *p = '\0';
  return out;
}

/*
** HELPER: init_registry
** ----------------------------------------------------------------------------
** Creates the internal bookkeeping table '_mview_registry' inside the
** attached cache database. This table remembers the SQL query for each view
** so we can refresh it later.
*/
static int init_registry(sqlite3 *db, char **err_msg) {
  const char *sql =
      "CREATE TABLE IF NOT EXISTS " CACHE_SCHEMA "._mview_registry ("
      "  view_name TEXT PRIMARY KEY,"
      "  source_query TEXT NOT NULL,"
      "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
      "  last_refreshed DATETIME"
      ");";
  return sqlite3_exec(db, sql, 0, 0, err_msg);
}

/*
** FUNCTION: mview_init
** SQL USAGE: SELECT mview_init('filename.db');
** ----------------------------------------------------------------------------
** 1. Checks if 'mviews' (CACHE_SCHEMA) is already attached.
** 2. If not attached, attaches the provided filename.
** 3. Ensures WAL mode is set (idempotent).
** 4. Creates the registry table if missing.
*/
static void mview_init_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *path = (const char *)sqlite3_value_text(argv[0]);
  char *err_msg = NULL;

  if (!path) {
    sqlite3_result_error(context, "Path argument is required", -1);
    return;
  }

  // 1. Check if the database alias is already attached.
  // sqlite3_db_filename returns NULL if the schema name is not found.
  const char *existing_schema_file = sqlite3_db_filename(db, CACHE_SCHEMA);
  int is_attached = (existing_schema_file != NULL);

  // 2. Attach if not already attached
  if (!is_attached) {
    // Use %Q to safely quote the file path (prevents injection via filename)
    char *attach_sql =
        sqlite3_mprintf("ATTACH DATABASE %Q AS " CACHE_SCHEMA, path);
    int rc = sqlite3_exec(db, attach_sql, 0, 0, &err_msg);
    sqlite3_free(attach_sql);

    if (rc != SQLITE_OK) {
      sqlite3_result_error(context, err_msg, -1);
      sqlite3_free(err_msg);
      return;
    }
  }

  // 3. Ensure WAL mode is set.
  // We run this regardless of whether we just attached or it was already attached.
  // SQLite handles this gracefully (if already WAL, it returns "wal" and does nothing).
  sqlite3_exec(db, "PRAGMA " CACHE_SCHEMA ".journal_mode=WAL", 0, 0, 0);

  // 4. Initialize the registry table (Idempotent via IF NOT EXISTS)
  if (init_registry(db, &err_msg) != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    return;
  }

  // Return specific status message
  if (is_attached) {
    sqlite3_result_text(context, "MView Storage Ready (Already Attached)", -1,
                        SQLITE_TRANSIENT);
  } else {
    sqlite3_result_text(context, "MView Storage Initialized (Attached)", -1,
                        SQLITE_TRANSIENT);
  }
}

/*
** FUNCTION: mview_create
** SQL USAGE: SELECT mview_create('view_name', 'select_sql',
* ['optional_schema']);
** ----------------------------------------------------------------------------
** Creates a new Materialized View.
** Steps:
** 1. Verify registry exists.
** 2. Start Transaction.
** 3. Save query metadata to registry.
** 4. Drop existing table (if any).
** 5. Create new table (CTAS or Strict Schema).
** 6. Commit.
*/
static void mview_create_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  const char *query = (const char *)sqlite3_value_text(argv[1]);
  // Optional 3rd argument for strict schema definition
  const char *schema =
      (argc > 2) ? (const char *)sqlite3_value_text(argv[2]) : NULL;

  char *sql = NULL;
  char *err_msg = NULL;

  // Sanity Check: Has mview_init() been called?
  if (init_registry(db, &err_msg) != SQLITE_OK) {
    sqlite3_free(err_msg);
    sqlite3_result_error(context, "Cache DB not found. Run mview_init() first.",
                         -1);
    return;
  }

  // Quote the name to prevent SQL injection in the table name
  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  /* BEGIN TRANSACTION */
  sqlite3_exec(db, "BEGIN TRANSACTION", 0, 0, 0);

  // 1. Update Registry
  // %q is used for string values (prevents escaping out of the string)
  char *reg_sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO " CACHE_SCHEMA
      "._mview_registry (view_name, source_query, last_refreshed) "
      "VALUES ('%q', '%q', CURRENT_TIMESTAMP)",
      name, query);

  if (sqlite3_exec(db, reg_sql, 0, 0, &err_msg) != SQLITE_OK) {
    sqlite3_free(reg_sql);
    goto error_rollback;
  }
  sqlite3_free(reg_sql);

  // 2. Drop Old Table (Cleanup)
  // %s is used because quoted_name is already safely formatted
  char *drop_sql =
      sqlite3_mprintf("DROP TABLE IF EXISTS " CACHE_SCHEMA ".%s", quoted_name);
  sqlite3_exec(db, drop_sql, 0, 0, 0);
  sqlite3_free(drop_sql);

  // 3. Create New Table
  if (schema && strlen(schema) > 0) {
    // MODE A: Strict Schema
    // Useful when defining PRIMARY KEYs or specific column types.
    // Step 3a: Create empty table
    sql = sqlite3_mprintf("CREATE TABLE " CACHE_SCHEMA ".%s (%s)", quoted_name,
                          schema);
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK)
      goto error_rollback;
    sqlite3_free(sql);

    // Step 3b: Insert data
    sql = sqlite3_mprintf("INSERT INTO " CACHE_SCHEMA ".%s SELECT * FROM (%s)",
                          quoted_name, query);
  } else {
    // MODE B: Auto Schema (Create Table As Select)
    // Easiest mode; SQLite infers types.
    sql = sqlite3_mprintf("CREATE TABLE " CACHE_SCHEMA ".%s AS %s", quoted_name,
                          query);
  }

  if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK)
    goto error_rollback;

  /* COMMIT */
  sqlite3_exec(db, "COMMIT", 0, 0, 0);
  sqlite3_free(sql);
  sqlite3_free(quoted_name);

  char *msg = sqlite3_mprintf("Memorized view '%s' created.", name);
  sqlite3_result_text(context, msg, -1, SQLITE_TRANSIENT);
  sqlite3_free(msg);
  return;

error_rollback:
  sqlite3_free(quoted_name);
  sqlite3_free(sql);
  sqlite3_result_error(context, err_msg, -1);
  sqlite3_free(err_msg);
  sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
}

/*
** FUNCTION: mview_refresh
** SQL USAGE: SELECT mview_refresh('view_name');
** ----------------------------------------------------------------------------
** Updates the data in an existing view.
** Steps:
** 1. Lookup the source query from the registry.
** 2. Start Transaction.
** 3. Delete all existing rows.
** 4. Insert new rows by running the source query.
** 5. Commit.
**
** Note: This is an "Atomic Refresh". The view is never empty to other readers
** (if WAL mode is on) or at least consistent (via transaction).
*/
static void mview_refresh_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  // 1. Lookup Source Query
  char *lookup_sql = sqlite3_mprintf("SELECT source_query FROM " CACHE_SCHEMA
                                     "._mview_registry WHERE view_name = '%q'",
                                     name);
  sqlite3_stmt *stmt;

  if (sqlite3_prepare_v2(db, lookup_sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(lookup_sql);
    sqlite3_result_error(context, "DB Error", -1);
    return;
  }
  sqlite3_free(lookup_sql);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }
  const char *query = (const char *)sqlite3_column_text(stmt, 0);
  // Create a copy of the query string before finalizing the statement
  char *saved_query = sqlite3_mprintf("%s", query ? query : "");
  sqlite3_finalize(stmt);

  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_free(saved_query);
    sqlite3_result_error_nomem(context);
    return;
  }

  char *err_msg = NULL;
  /* BEGIN TRANSACTION */
  sqlite3_exec(db, "BEGIN TRANSACTION", 0, 0, 0);

  // 2. Wipe old data
  char *del_sql =
      sqlite3_mprintf("DELETE FROM " CACHE_SCHEMA ".%s", quoted_name);
  sqlite3_exec(db, del_sql, 0, 0, 0);
  sqlite3_free(del_sql);

  // 3. Insert new data
  // We wrap query in parenthesis: SELECT * FROM (source_query)
  // This handles complex source queries (like UNIONS) correctly.
  char *ins_sql =
      sqlite3_mprintf("INSERT INTO " CACHE_SCHEMA ".%s SELECT * FROM (%s)",
                      quoted_name, saved_query);
  int rc = sqlite3_exec(db, ins_sql, 0, 0, &err_msg);
  sqlite3_free(ins_sql);

  // 4. Update Timestamp
  if (rc == SQLITE_OK) {
    char *upd_sql = sqlite3_mprintf("UPDATE " CACHE_SCHEMA
                                    "._mview_registry SET last_refreshed = "
                                    "CURRENT_TIMESTAMP WHERE view_name = '%q'",
                                    name);
    sqlite3_exec(db, upd_sql, 0, 0, 0);
    sqlite3_free(upd_sql);
  }

  sqlite3_free(saved_query);
  sqlite3_free(quoted_name);

  if (rc != SQLITE_OK) {
    /* ROLLBACK on failure (restores deleted data) */
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
  } else {
    /* COMMIT on success */
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
    sqlite3_result_text(context, "Refreshed", -1, SQLITE_TRANSIENT);
  }
}

/*
** FUNCTION: mview_drop
** SQL USAGE: SELECT mview_drop('view_name');
** ----------------------------------------------------------------------------
** Permanently removes a view.
** 1. Deletes metadata from registry.
** 2. Drops the physical table.
*/
static void mview_drop_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  char *err_msg = NULL;

  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION", 0, 0, 0);

  // 1. Remove Registry Entry
  char *del_reg = sqlite3_mprintf("DELETE FROM " CACHE_SCHEMA
                                  "._mview_registry WHERE view_name = '%q'",
                                  name);
  if (sqlite3_exec(db, del_reg, 0, 0, &err_msg) != SQLITE_OK) {
    sqlite3_free(del_reg);
    goto error;
  }
  sqlite3_free(del_reg);

  // 2. Drop Physical Table
  char *drop_tbl =
      sqlite3_mprintf("DROP TABLE IF EXISTS " CACHE_SCHEMA ".%s", quoted_name);
  int rc = sqlite3_exec(db, drop_tbl, 0, 0, &err_msg);
  sqlite3_free(drop_tbl);

  if (rc != SQLITE_OK)
    goto error;

  sqlite3_exec(db, "COMMIT", 0, 0, 0);
  sqlite3_free(quoted_name);
  sqlite3_result_text(context, "Dropped", -1, SQLITE_TRANSIENT);
  return;

error:
  sqlite3_free(quoted_name);
  sqlite3_result_error(context, err_msg, -1);
  sqlite3_free(err_msg);
  sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
}

/*
** FUNCTION: mview_close
** SQL USAGE: SELECT mview_close();
** ----------------------------------------------------------------------------
** Detaches the cache database.
** - This cleanly disconnects the "mviews" schema.
** - The physical file 'cache.db' remains on disk (data persists).
** - Use this to clean up before closing your application or if you want
**   to switch cache files.
*/
static void mview_close_func(sqlite3_context *context, int argc,
                             sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  char *err_msg = NULL;

  char *detach_sql = sqlite3_mprintf("DETACH DATABASE " CACHE_SCHEMA);
  int rc = sqlite3_exec(db, detach_sql, 0, 0, &err_msg);
  sqlite3_free(detach_sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
  } else {
    sqlite3_result_text(context, "MView Cache Detached", -1, SQLITE_TRANSIENT);
  }
}

/*
** ENTRY POINT: sqlite3_extension_init
** ----------------------------------------------------------------------------
** This function is called automatically when SQLite loads the extension.
** It registers the custom SQL functions.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
                           const sqlite3_api_routines *pApi) {
  // 1. Initialize API pointers (Critical for extension to work)
  SQLITE_EXTENSION_INIT2(pApi);

  // 2. Register Functions
  // Format: create_function(db, name, num_args, encoding, data, func_ptr, step,
  // final)

  // mview_init('filename')
  sqlite3_create_function(db, "mview_init", 1, SQLITE_UTF8, 0, mview_init_func,
                          0, 0);

  // mview_create('name', 'query') - 2 args
  sqlite3_create_function(db, "mview_create", 2, SQLITE_UTF8, 0,
                          mview_create_func, 0, 0);

  // mview_create('name', 'query', 'schema') - 3 args (Overloaded)
  sqlite3_create_function(db, "mview_create", 3, SQLITE_UTF8, 0,
                          mview_create_func, 0, 0);

  // mview_refresh('name')
  sqlite3_create_function(db, "mview_refresh", 1, SQLITE_UTF8, 0,
                          mview_refresh_func, 0, 0);

  // mview_drop('name')
  sqlite3_create_function(db, "mview_drop", 1, SQLITE_UTF8, 0, mview_drop_func,
                          0, 0);

  // mview_close()
  sqlite3_create_function(db, "mview_close", 0, SQLITE_UTF8, 0,
                          mview_close_func, 0, 0);

  return 0;
}
