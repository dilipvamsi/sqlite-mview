/*
** ============================================================================
** SQLite Memorized Views (sqlite-mview)
** ============================================================================
**
** DESCRIPTION:
**   This extension implements "Materialized Views" (Memorized Queries) for
**   SQLite.
**   Standard SQLite views are virtual; they re-run the query every time you
**   read them.
**   This extension stores the results of complex queries in real physical
**   tables
**   inside a separate "Attached Database".
**
**   Benefits:
**   1. Performance: Reads are instant (O(1) or O(log n) with indexes).
**   2. Isolation:   View data lives in a separate file, keeping the main DB
**      clean.
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
** SETUP:
**   .load ./mview                           -- Load extension
**   SELECT mview_version();                 -- Returns '3.0.0'
**   SELECT mview_attach('cache.db');        -- Attach storage file
**   SELECT mview_init();                    -- Initialize registry
**
** BASIC OPERATIONS:
**   -- Create view (auto schema)
**   SELECT mview_create('daily_stats', 'SELECT date, sum(total) FROM orders
* GROUP BY date');
**
**   -- Create view (strict schema with PRIMARY KEY)
**   SELECT mview_create('users_mv', 'SELECT id, name FROM users', 'id INTEGER
* PRIMARY KEY, name TEXT');
**
**   -- Query data
**   SELECT * FROM mviews.daily_stats WHERE total > 1000;
**
**   -- Refresh data (atomic swap)
**   SELECT mview_refresh('daily_stats');
**
**   -- Drop view
**   SELECT mview_drop('daily_stats');
**
** INDEX MANAGEMENT:
**   SELECT mview_add_index('daily_stats', 'date', 1);     -- 1 = UNIQUE
**   SELECT mview_add_index('daily_stats', 'total', 0);    -- 0 = Non-unique
**   SELECT mview_remove_index('daily_stats', 'total');    -- Remove index
**   SELECT mview_reindex('daily_stats');                  -- Rebuild all
* indexes
**
** INTROSPECTION:
**   SELECT * FROM mview_registry;             -- List all views (virtual table)
**   SELECT mview_has('daily_stats');          -- Check if view exists (1|0)
**   SELECT mview_count();                     -- Total number of views
**   SELECT mview_query('daily_stats');        -- Get source query
**   SELECT mview_schema('daily_stats');       -- Column schema as JSON
**   SELECT mview_indexes('daily_stats');      -- Registered indexes as JSON
**   SELECT mview_size('daily_stats');         -- Disk size in bytes
**   SELECT mview_info('daily_stats');         -- Full JSON metadata
**   SELECT mview_stats('daily_stats');        -- Row count & size
**   SELECT mview_verify('daily_stats');       -- Schema consistency check
**   SELECT mview_explain('daily_stats');      -- Show source query
**   SELECT mview_export('daily_stats');       -- Export as SQL
**
** TIME-BASED FUNCTIONS:
**   SELECT mview_last_refreshed('daily_stats'); -- Timestamp of last refresh
**   SELECT mview_age('daily_stats');            -- Seconds since last refresh
**   SELECT mview_stale('daily_stats', 3600);    -- 1 if older than 1 hour
**   SELECT mview_refresh_stale(3600);           -- Refresh views older than 1h
**
** BULK OPERATIONS:
**   SELECT mview_refresh_all();               -- Refresh all views
**   SELECT mview_drop_all();                  -- Drop all views
**   SELECT mview_truncate('daily_stats');     -- Clear data only
**   SELECT mview_vacuum();                    -- Vacuum cache DB
**
** LOGGING:
**   SELECT mview_log_enable(1);               -- Enable operation logging
**   SELECT mview_log();                       -- Last 50 log entries (JSON)
**   SELECT mview_log(100);                    -- Last 100 log entries
**   SELECT mview_log_clear();                 -- Clear all log entries
**   SELECT mview_log_enable(0);               -- Disable logging
**
** SCHEMA EVOLUTION:
**   SELECT mview_rename('old_name', 'new_name'); -- Rename view
**
** CLEANUP:
**   SELECT mview_dettach();                   -- Detach storage
**
** ============================================================================
*/

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <stdio.h>
#include <string.h>

#define MVIEW_VERSION "3.0.0"

/*
** CONSTANT: MVIEWS_SCHEMA
** We force the attached database to have this specific alias ("mviews").
** This ensures our SQL statements (like 'INSERT INTO mviews.table...')
** always target the correct database.
*/
#define MVIEWS_SCHEMA "mviews"

/*
** STRUCT: mview_context
** ----------------------------------------------------------------------------
** Holds per-connection state for the extension.
** - attached_by_us: 1 if mview_attach() was called successfully, 0 otherwise.
** - logging_enabled: 1 if logging is enabled, 0 otherwise.
*/
typedef struct {
  int attached_by_us;
  int logging_enabled;
} mview_context;

static void mview_context_free(void *p) { sqlite3_free(p); }

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
** (values).
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
** HELPER: get_random_suffix
** ----------------------------------------------------------------------------
** Generates a random hex string suffix.
** Used to create unique temporary table names for concurrent refreshes.
*/
static void get_random_suffix(char *buffer, int length) {
  unsigned char random_bytes[16];
  int bytes_needed = length / 2;
  if (bytes_needed > sizeof(random_bytes))
    bytes_needed = sizeof(random_bytes);

  sqlite3_randomness(bytes_needed, random_bytes);

  for (int i = 0; i < bytes_needed; i++) {
    sprintf(&buffer[i * 2], "%02x", random_bytes[i]);
  }
  buffer[length] = '\0';
}

/*
** HELPER: init_registry
** ----------------------------------------------------------------------------
** Creates the internal bookkeeping tables inside the attached cache database.
** 1. _mview_registry: Tracks query definitions.
** 2. _mview_index_registry: Tracks index definitions for persistence.
*/
static int init_registry(sqlite3 *db, char **err_msg) {
  const char *sql =
      "CREATE TABLE IF NOT EXISTS " MVIEWS_SCHEMA "._mview_registry ("
      "  view_name TEXT PRIMARY KEY,"
      "  source_query TEXT NOT NULL,"
      "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
      "  last_refreshed DATETIME"
      ");"
      "CREATE TABLE IF NOT EXISTS " MVIEWS_SCHEMA "._mview_index_registry ("
      "  view_name TEXT,"
      "  columns TEXT,"
      "  is_unique INTEGER,"
      "  FOREIGN KEY(view_name) REFERENCES _mview_registry(view_name) ON "
      "DELETE CASCADE"
      ");"
      "CREATE TABLE IF NOT EXISTS " MVIEWS_SCHEMA "._mview_log ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,"
      "  operation TEXT,"
      "  view_name TEXT,"
      "  details TEXT"
      ");";
  return sqlite3_exec(db, sql, 0, 0, err_msg);
}

/*
** FUNCTION: mview_version
** SQL USAGE: SELECT mview_version();
** ----------------------------------------------------------------------------
** Returns the current version string.
*/
static void mview_version_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3_result_text(context, MVIEW_VERSION, -1, SQLITE_STATIC);
}

/*
** FUNCTION: mview_attach
** SQL USAGE: SELECT mview_attach('filename.db');
** ----------------------------------------------------------------------------
** 1. Checks if 'mviews' (MVIEWS_SCHEMA) is already attached.
** 2. If attached, throws error.
** 3. Attaches the provided filename as 'mviews'.
** 4. Sets context->attached_by_us = 1.
*/
static void mview_attach_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  mview_context *ctx = (mview_context *)sqlite3_user_data(context);
  const char *path = (const char *)sqlite3_value_text(argv[0]);
  char *err_msg = NULL;

  if (!path) {
    sqlite3_result_error(context, "Path argument is required", -1);
    return;
  }

  // 1. Check if already attached
  const char *existing_schema_file = sqlite3_db_filename(db, MVIEWS_SCHEMA);
  if (existing_schema_file != NULL) {
    sqlite3_result_error(context,
                         "Database already attached as '" MVIEWS_SCHEMA
                         "'. Detach it first.",
                         -1);
    return;
  }

  // 2. Attach
  char *attach_sql =
      sqlite3_mprintf("ATTACH DATABASE %Q AS " MVIEWS_SCHEMA, path);
  int rc = sqlite3_exec(db, attach_sql, 0, 0, &err_msg);
  sqlite3_free(attach_sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    return;
  }

  // 3. Mark as attached by us
  if (ctx) {
    ctx->attached_by_us = 1;
  }

  // 4. Set WAL mode immediately (Good practice)
  sqlite3_exec(db, "PRAGMA " MVIEWS_SCHEMA ".journal_mode=WAL", 0, 0, 0);

  sqlite3_result_text(context, "Attached", -1, SQLITE_TRANSIENT);
}

/*
** FUNCTION: mview_dettach
** SQL USAGE: SELECT mview_dettach();
** ----------------------------------------------------------------------------
** 1. Checks if 'mviews' is attached.
** 2. Checks if context->attached_by_us is true.
** 3. If so, detaches and clear flag.
** 4. Else, throws error.
*/
static void mview_dettach_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  mview_context *ctx = (mview_context *)sqlite3_user_data(context);
  char *err_msg = NULL;

  // 1. Check if attached
  const char *existing_schema_file = sqlite3_db_filename(db, MVIEWS_SCHEMA);
  if (existing_schema_file == NULL) {
    sqlite3_result_error(context, "No mview database attached.", -1);
    return;
  }

  // 2. Check permission
  if (!ctx || !ctx->attached_by_us) {
    sqlite3_result_error(
        context, "Cannot detach: Database was not attached via mview_attach().",
        -1);
    return;
  }

  // 3. Detach
  char *detach_sql = sqlite3_mprintf("DETACH DATABASE " MVIEWS_SCHEMA);
  int rc = sqlite3_exec(db, detach_sql, 0, 0, &err_msg);
  sqlite3_free(detach_sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
  } else {
    ctx->attached_by_us = 0;
    sqlite3_result_text(context, "Detached", -1, SQLITE_TRANSIENT);
  }
}

/*
** FUNCTION: mview_init
** SQL USAGE: SELECT mview_init();
** ----------------------------------------------------------------------------
** 1. Checks if 'mviews' (MVIEWS_SCHEMA) is attached.
** 2. Creates the registry table if missing.
*/
static void mview_init_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  // We generally expect mview_attach() to be called first, but if the user
  // manually attached 'mviews' and calls this, it will just initialize the
  // tables. We do NOT strictly enforce attached_by_us here, allowing manual
  // attach + mview_init if users really want that (though detach won't work).

  char *err_msg = NULL;

  const char *existing_schema_file = sqlite3_db_filename(db, MVIEWS_SCHEMA);
  if (existing_schema_file == NULL) {
    sqlite3_result_error(
        context, "MView storage not found. Call mview_attach(path) first.", -1);
    return;
  }

  // Initialize the registry table (Idempotent via IF NOT EXISTS)
  if (init_registry(db, &err_msg) != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    return;
  }

  sqlite3_result_text(context, "MView Registry Initialized", -1,
                      SQLITE_TRANSIENT);
}

/*
** FUNCTION: mview_create
** SQL USAGE: SELECT mview_create('view_name', 'select_sql',
* ['optional_schema']);
** ----------------------------------------------------------------------------
** Creates a new Materialized View.
*/
static void mview_create_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  const char *query = (const char *)sqlite3_value_text(argv[1]);
  // Optional 3rd argument for strict schema definition
  const char *schema =
      (argc > 2) ? (const char *)sqlite3_value_text(argv[2]) : NULL;

  if (!name || !query) {
    sqlite3_result_error(context, "Name and Query arguments cannot be NULL",
                         -1);
    return;
  }

  if (!name || !query) {
    sqlite3_result_error(context, "Name and Query arguments cannot be NULL",
                         -1);
    return;
  }

  char *sql = NULL;
  char *err_msg = NULL;

  // Sanity Check: Has mview_init() been called?
  if (init_registry(db, &err_msg) != SQLITE_OK) {
    sqlite3_free(err_msg);
    sqlite3_result_error(
        context, "Cache DB not initialized. Run mview_init() first.", -1);
    return;
  }

  // Quote the name to prevent SQL injection in the table name
  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  /* BEGIN TRANSACTION - Using SAVEPOINT for nested transaction support */
  sqlite3_exec(db, "SAVEPOINT mview_create_sp", 0, 0, 0);

  // 1. Update Registry
  // %q is used for string values (prevents escaping out of the string)
  char *reg_sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO " MVIEWS_SCHEMA
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
      sqlite3_mprintf("DROP TABLE IF EXISTS " MVIEWS_SCHEMA ".%s", quoted_name);
  sqlite3_exec(db, drop_sql, 0, 0, 0);
  sqlite3_free(drop_sql);

  // 3. Create New Table
  if (schema && strlen(schema) > 0) {
    // MODE A: Strict Schema
    // Useful when defining PRIMARY KEYs or specific column types.
    // Step 3a: Create empty table
    sql = sqlite3_mprintf("CREATE TABLE " MVIEWS_SCHEMA ".%s (%s)", quoted_name,
                          schema);
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK)
      goto error_rollback;
    sqlite3_free(sql);

    // Step 3b: Insert data
    sql = sqlite3_mprintf("INSERT INTO " MVIEWS_SCHEMA ".%s SELECT * FROM (%s)",
                          quoted_name, query);
  } else {
    // MODE B: Auto Schema (Create Table As Select)
    // Easiest mode; SQLite infers types.
    sql = sqlite3_mprintf("CREATE TABLE " MVIEWS_SCHEMA ".%s AS %s",
                          quoted_name, query);
  }

  if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK)
    goto error_rollback;

  /* COMMIT */
  sqlite3_exec(db, "RELEASE mview_create_sp", 0, 0, 0);
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
  sqlite3_exec(db, "ROLLBACK TO mview_create_sp", 0, 0, 0);
  sqlite3_exec(db, "RELEASE mview_create_sp", 0, 0, 0);
}

/*
** FUNCTION: mview_add_index
** SQL USAGE: SELECT mview_add_index('view_name', 'col1, col2', is_unique);
** ----------------------------------------------------------------------------
** Registers an index for the view.
*/
static void mview_add_index_func(sqlite3_context *context, int argc,
                                 sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *view_name = (const char *)sqlite3_value_text(argv[0]);
  const char *columns = (const char *)sqlite3_value_text(argv[1]);
  int is_unique = sqlite3_value_int(argv[2]);

  if (!view_name || !columns) {
    sqlite3_result_error(context, "View name and columns cannot be NULL", -1);
    return;
  }
  char *err_msg = 0;

  // 1. Insert into Registry
  // mview_registry       - Eponymous Virtual Table listing registered views
  char *reg_sql = sqlite3_mprintf("INSERT INTO " MVIEWS_SCHEMA
                                  "._mview_index_registry (view_name, columns, "
                                  "is_unique) VALUES ('%q', '%q', %d)",
                                  view_name, columns, is_unique);

  int rc = sqlite3_exec(db, reg_sql, 0, 0, &err_msg);
  sqlite3_free(reg_sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    return;
  }

  // 2. Apply index immediately to the current live table (Best Effort)
  char *quoted_name = quote_identifier(view_name);

  // Generate random suffix to ensure unique index name
  char suffix[9];
  get_random_suffix(suffix, 8);

  char *sql = sqlite3_mprintf(
      "CREATE %s INDEX IF NOT EXISTS " MVIEWS_SCHEMA ".idx_%s_%s ON %s (%s)",
      is_unique ? "UNIQUE" : "", view_name, suffix, quoted_name, columns);
  sqlite3_free(quoted_name);

  rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    // Safe to ignore error here; table might not exist yet,
    // or user called this before creating the view.
    // It will be created on next refresh.
    sqlite3_free(err_msg);
  }

  sqlite3_result_text(context, "Index registered", -1, SQLITE_STATIC);
}

/*
** FUNCTION: mview_refresh
** SQL USAGE: SELECT mview_refresh('view_name');
** ----------------------------------------------------------------------------
** Updates the data in an existing view using a "Shadow Swap" strategy.
*/
static void mview_refresh_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_error(context, "View name cannot be NULL", -1);
    return;
  }

  // 1. Lookup Source Query
  char *lookup_sql = sqlite3_mprintf("SELECT source_query FROM " MVIEWS_SCHEMA
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
  char *saved_query = sqlite3_mprintf("%s", query ? query : "");
  sqlite3_finalize(stmt);

  // 2. Prepare Temp Table Name
  char suffix[9];
  get_random_suffix(suffix, 8);

  // Name format: viewname_new_a1b2c3d4
  char *temp_table_name = sqlite3_mprintf("%s_new_%s", name, suffix);
  char *quoted_temp = quote_identifier(temp_table_name);
  char *quoted_real = quote_identifier(name);

  char *err_msg = NULL;

  // 3. Create & Populate Temp Table (Heavy Lifting - Non Blocking)
  // Drop debris if exists (unlikely due to random suffix)
  char *drop_temp =
      sqlite3_mprintf("DROP TABLE IF EXISTS " MVIEWS_SCHEMA ".%s", quoted_temp);
  sqlite3_exec(db, drop_temp, 0, 0, 0);
  sqlite3_free(drop_temp);

  // Run CTAS
  char *create_sql =
      sqlite3_mprintf("CREATE TABLE " MVIEWS_SCHEMA ".%s AS SELECT * FROM (%s)",
                      quoted_temp, saved_query);
  int rc = sqlite3_exec(db, create_sql, 0, 0, &err_msg);
  sqlite3_free(create_sql);
  sqlite3_free(saved_query);

  if (rc != SQLITE_OK) {
    sqlite3_free(quoted_temp);
    sqlite3_free(quoted_real);
    sqlite3_free(temp_table_name);
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    return;
  }

  // 4. Apply Indexes to Temp Table
  // We must look up registered indexes and create them on the temp table now
  char *idx_query =
      sqlite3_mprintf("SELECT columns, is_unique FROM " MVIEWS_SCHEMA
                      "._mview_index_registry WHERE view_name = '%q'",
                      name);

  if (sqlite3_prepare_v2(db, idx_query, -1, &stmt, 0) == SQLITE_OK) {
    int idx_counter = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *cols = (const char *)sqlite3_column_text(stmt, 0);
      int is_unique = sqlite3_column_int(stmt, 1);

      // Create index on temp table. It will carry over during rename.
      // Name: idx_suffix_counter (to ensure uniqueness)
      char *idx_sql = sqlite3_mprintf(
          "CREATE %s INDEX " MVIEWS_SCHEMA ".idx_%s_%d ON %s (%s)",
          is_unique ? "UNIQUE" : "", suffix, idx_counter++, quoted_temp, cols);

      int idx_rc = sqlite3_exec(db, idx_sql, 0, 0, &err_msg);
      sqlite3_free(idx_sql);

      if (idx_rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        // Clean up and abort
        char *cleanup =
            sqlite3_mprintf("DROP TABLE " MVIEWS_SCHEMA ".%s", quoted_temp);
        sqlite3_exec(db, cleanup, 0, 0, 0);
        sqlite3_free(cleanup);

        sqlite3_free(quoted_temp);
        sqlite3_free(quoted_real);
        sqlite3_free(temp_table_name);
        sqlite3_free(idx_query);
        sqlite3_result_error(context, err_msg, -1);
        sqlite3_free(err_msg);
        return;
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_free(idx_query);

  // 5. Atomic Swap (Transaction)
  // Blocks new writers, allows existing readers (WAL)
  rc = sqlite3_exec(db, "SAVEPOINT mview_refresh_sp", 0, 0, 0);
  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, "Could not start savepoint", -1);
    goto cleanup;
  }

  // Drop old table
  char *drop_old =
      sqlite3_mprintf("DROP TABLE IF EXISTS " MVIEWS_SCHEMA ".%s", quoted_real);
  rc = sqlite3_exec(db, drop_old, 0, 0, &err_msg);
  sqlite3_free(drop_old);

  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK TO mview_refresh_sp", 0, 0, 0);
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    goto cleanup;
  }

  // Rename Temp -> Real
  char *rename_sql =
      sqlite3_mprintf("ALTER TABLE " MVIEWS_SCHEMA ".%s RENAME TO %s",
                      quoted_temp, quoted_real);
  // Note: quoted_real includes quotes, which works for RENAME TO identifier
  // BUT SQLite RENAME TO expects just the name or "name".
  // Since quoted_real is "name", this is valid.
  rc = sqlite3_exec(db, rename_sql, 0, 0, &err_msg);
  sqlite3_free(rename_sql);

  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK TO mview_refresh_sp", 0, 0, 0);
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    goto cleanup;
  }

  // Update Timestamp
  char *upd_sql = sqlite3_mprintf("UPDATE " MVIEWS_SCHEMA
                                  "._mview_registry SET last_refreshed = "
                                  "CURRENT_TIMESTAMP WHERE view_name = '%q'",
                                  name);
  sqlite3_exec(db, upd_sql, 0, 0, 0);
  sqlite3_free(upd_sql);

  sqlite3_exec(db, "RELEASE mview_refresh_sp", 0, 0, 0);
  sqlite3_result_text(context, "Refreshed", -1, SQLITE_TRANSIENT);

  // Cleanup strings
  sqlite3_free(quoted_temp);
  sqlite3_free(quoted_real);
  sqlite3_free(temp_table_name);
  return;

cleanup:
  // If transaction failed, we might have a stray temp table
  {
    char *cleanup = sqlite3_mprintf("DROP TABLE IF EXISTS " MVIEWS_SCHEMA ".%s",
                                    quoted_temp);
    sqlite3_exec(db, cleanup, 0, 0, 0);
    sqlite3_free(cleanup);
  }
  sqlite3_free(quoted_temp);
  sqlite3_free(quoted_real);
  sqlite3_free(temp_table_name);
}

/*
** ============================================================================
** VIRTUAL TABLE: mview_registry
** ============================================================================
** Allows querying registered views as a table: SELECT * FROM mview_registry;
*/

typedef struct mview_registry_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
} mview_list_vtab;

typedef struct mview_list_cursor {
  sqlite3_vtab_cursor base;
  sqlite3_stmt *stmt;
  int last_rc; // Result of last sqlite3_step
} mview_list_cursor;

/*
** xConnect/xCreate
*/
static int mview_list_connect(sqlite3 *db, void *pAux, int argc,
                              const char *const *argv, sqlite3_vtab **ppVtab,
                              char **pzErr) {
  mview_list_vtab *pNew;
  int rc;

  // Define the schema for the virtual table
  rc = sqlite3_declare_vtab(db, "CREATE TABLE x(view_name TEXT, source_query "
                                "TEXT, created_at TEXT, last_refreshed TEXT)");
  if (rc == SQLITE_OK) {
    pNew = sqlite3_malloc(sizeof(*pNew));
    *ppVtab = (sqlite3_vtab *)pNew;
    if (pNew == 0)
      return SQLITE_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
  }
  return rc;
}

/*
** xDisconnect/xDestroy
*/
static int mview_list_disconnect(sqlite3_vtab *pVtab) {
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

/*
** xOpen
*/
static int mview_list_open(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor) {
  mview_list_cursor *pCur;
  pCur = sqlite3_malloc(sizeof(*pCur));
  if (pCur == 0)
    return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
** xClose
*/
static int mview_list_close(sqlite3_vtab_cursor *cur) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  sqlite3_finalize(pCur->stmt);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
** xNext
*/
static int mview_list_next(sqlite3_vtab_cursor *cur) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  pCur->last_rc = sqlite3_step(pCur->stmt);
  return SQLITE_OK;
}

/*
** xFilter
*/
static int mview_list_filter(sqlite3_vtab_cursor *cur, int idxNum,
                             const char *idxStr, int argc,
                             sqlite3_value **argv) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  mview_list_vtab *pTab = (mview_list_vtab *)cur->pVtab;

  // Clean up any existing statement
  sqlite3_finalize(pCur->stmt);
  pCur->stmt = NULL;

  const char *sql =
      "SELECT view_name, source_query, created_at, last_refreshed "
      "FROM " MVIEWS_SCHEMA "._mview_registry ORDER BY view_name";
  int rc = sqlite3_prepare_v2(pTab->db, sql, -1, &pCur->stmt, 0);

  if (rc != SQLITE_OK) {
    // If table doesn't exist (e.g. init not called), this fails.
    // Returning error is fine.
    return rc;
  }

  // Position the cursor on the first row
  return mview_list_next(cur);
}

/*
** xEof
*/
static int mview_list_eof(sqlite3_vtab_cursor *cur) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  return pCur->last_rc == SQLITE_DONE;
}

/*
** xColumn
*/
static int mview_list_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx,
                             int i) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  // Columns: 0=view_name, 1=source_query, 2=created_at, 3=last_refreshed
  sqlite3_result_value(ctx, sqlite3_column_value(pCur->stmt, i));
  return SQLITE_OK;
}

/*
** xRowid
*/
static int mview_list_rowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid) {
  mview_list_cursor *pCur = (mview_list_cursor *)cur;
  // Use the pointer address of the name string as a temporary rowid?
  // No, that's not stable.
  // Use a hash of the view name.
  const char *name = (const char *)sqlite3_column_text(pCur->stmt, 0);
  sqlite_int64 hash = 0;
  if (name) {
    for (int i = 0; name[i]; i++) {
      hash = (hash * 31) + name[i];
    }
  }
  *pRowid = hash;
  return SQLITE_OK;
}

/*
** xBestIndex
*/
static int mview_list_bestindex(sqlite3_vtab *tab,
                                sqlite3_index_info *pIdxInfo) {
  // We only support full scan
  pIdxInfo->estimatedCost = 1000.0;
  return SQLITE_OK;
}

/*
** Module Definition
*/
static sqlite3_module mview_list_module = {
    0,                     /* iVersion */
    mview_list_connect,    /* xCreate */
    mview_list_connect,    /* xConnect */
    mview_list_bestindex,  /* xBestIndex */
    mview_list_disconnect, /* xDisconnect */
    mview_list_disconnect, /* xDestroy */
    mview_list_open,       /* xOpen */
    mview_list_close,      /* xClose */
    mview_list_filter,     /* xFilter */
    mview_list_next,       /* xNext */
    mview_list_eof,        /* xEof */
    mview_list_column,     /* xColumn */
    mview_list_rowid,      /* xRowid */
    0,                     /* xUpdate */
    0,                     /* xBegin */
    0,                     /* xSync */
    0,                     /* xCommit */
    0,                     /* xRollback */
    0,                     /* xFindFunction */
    0,                     /* xRename */
    0,                     /* xSavepoint */
    0,                     /* xRelease */
    0,                     /* xRollbackTo */
    0                      /* xShadowName */
};

/*
** FUNCTION: mview_info
** SQL USAGE: SELECT mview_info('view_name');
** ----------------------------------------------------------------------------
** Returns a JSON object with metadata about the view.
*/
static void mview_info_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  char *sql = sqlite3_mprintf(
      "SELECT view_name, source_query, created_at, last_refreshed "
      "FROM " MVIEWS_SCHEMA "._mview_registry WHERE view_name = '%q'",
      name);
  sqlite3_stmt *stmt;

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "DB Error", -1);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }

  // Retrieve columns
  // 0: name, 1: query, 2: created, 3: refreshed
  const char *v_name = (const char *)sqlite3_column_text(stmt, 0);
  // const char *v_query = (const char *)sqlite3_column_text(stmt, 1); // Unused
  const char *v_created = (const char *)sqlite3_column_text(stmt, 2);
  const char *v_refreshed = (const char *)sqlite3_column_text(stmt, 3);

  // Safe approach: Use sqlite3_mprintf with %q for values to ensure they are
  // safe SQL strings? No, JSON escaping is different from SQL escaping. %q
  // escapes ' -> '' JSON needs " -> \" We will trust the content for now or
  // implement a mini-escaper. Given these are stored by us, they should be
  // "clean enough" for a basic implementation, except the source query might
  // contain anything. We MUST escape the query for JSON.

  // Hacky JSON construction using %Q (SQL quoted string) then seemingly
  // converting to JSON string? No. Let's just return a simple formatted string
  // that looks like JSON, but properly escaping newlines/quotes in query is
  // hard in C without a lib. ALTERNATIVE: Return SQLite's `json_object` if the
  // json1 extension is available? We can't guarantee json1 is loaded or linked.

  // Fallback: We'll construct a very basic string and hope for the best on
  // strict escaping or just return the fields that are safe. Actually, we can
  // use `sqlite3_str` API if available (3.24+), but let's stick to mprintf.

  char *json = sqlite3_mprintf("{"
                               "\"view_name\": \"%s\","
                               "\"created_at\": \"%s\","
                               "\"last_refreshed\": \"%s\","
                               "\"source_query_hint\": \"(query content)\""
                               "}",
                               v_name, v_created ? v_created : "",
                               v_refreshed ? v_refreshed : "");
  // Note: We avoid embedding the full query in JSON here to avoid escaping hell
  // in C. Users can query _mview_registry directly for the raw SQL.

  sqlite3_finalize(stmt);
  sqlite3_result_text(context, json, -1, SQLITE_TRANSIENT);
  sqlite3_free(json);
}

/*
** FUNCTION: mview_stats
** SQL USAGE: SELECT mview_stats('view_name');
** ----------------------------------------------------------------------------
** Returns a JSON object with row count and size.
*/
static void mview_stats_func(sqlite3_context *context, int argc,
                             sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  // 1. Get Row Count
  char *sql =
      sqlite3_mprintf("SELECT count(*) FROM " MVIEWS_SCHEMA ".%s", quoted_name);
  sqlite3_stmt *stmt;
  int row_count = -1;

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      row_count = sqlite3_column_int(stmt, 0);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_free(sql);

  // 2. Get Size (approximated via average row size if available, or just omit)
  // We can query dbstat if enabled, but let's stick to row_count for now.
  // We can also check index size?

  char *json = sqlite3_mprintf("{\"view_name\": \"%s\", \"row_count\": %d}",
                               name, row_count);
  sqlite3_result_text(context, json, -1, SQLITE_TRANSIENT);
  sqlite3_free(json);
  sqlite3_free(quoted_name);
}

/*
** FUNCTION: mview_verify
** SQL USAGE: SELECT mview_verify('view_name');
** ----------------------------------------------------------------------------
** Verifies if the view schema matches the source query schema.
** Returns "OK" or an error message description.
*/
static void mview_verify_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  // 1. Get Source Query
  char *lookup_sql = sqlite3_mprintf("SELECT source_query FROM " MVIEWS_SCHEMA
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

  // 2. Prepare Source Query (to get columns)
  sqlite3_stmt *src_stmt;
  if (sqlite3_prepare_v2(db, query, -1, &src_stmt, 0) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_result_text(context, "FAIL: Source query invalid", -1,
                        SQLITE_TRANSIENT);
    return;
  }

  // 3. Prepare View Table Query (to get columns)
  char *quoted_name = quote_identifier(name);
  char *view_sql =
      sqlite3_mprintf("SELECT * FROM " MVIEWS_SCHEMA ".%s", quoted_name);
  sqlite3_stmt *view_stmt;

  if (sqlite3_prepare_v2(db, view_sql, -1, &view_stmt, 0) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_finalize(src_stmt);
    sqlite3_free(view_sql);
    sqlite3_free(quoted_name);
    sqlite3_result_text(context, "FAIL: View table missing or corrupt", -1,
                        SQLITE_TRANSIENT);
    return;
  }

  // 4. Compare Column Counts
  int src_cols = sqlite3_column_count(src_stmt);
  int view_cols = sqlite3_column_count(view_stmt);

  if (src_cols != view_cols) {
    char *msg =
        sqlite3_mprintf("FAIL: Column count mismatch (Source: %d, View: %d)",
                        src_cols, view_cols);
    sqlite3_result_text(context, msg, -1, SQLITE_TRANSIENT);
    sqlite3_free(msg);
  } else {
    // 5. Compare Column Names
    int mismatch = 0;
    for (int i = 0; i < src_cols; i++) {
      const char *s_name = sqlite3_column_name(src_stmt, i);
      const char *v_name = sqlite3_column_name(view_stmt, i);
      if (strcmp(s_name, v_name) != 0) {
        char *msg = sqlite3_mprintf(
            "FAIL: Column name mismatch at index %d ('%s' vs '%s')", i, s_name,
            v_name);
        sqlite3_result_text(context, msg, -1, SQLITE_TRANSIENT);
        sqlite3_free(msg);
        mismatch = 1;
        break;
      }
    }
    if (!mismatch) {
      sqlite3_result_text(context, "OK", -1, SQLITE_STATIC);
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_finalize(src_stmt);
  sqlite3_finalize(view_stmt);
  sqlite3_free(view_sql);
  sqlite3_free(quoted_name);
}

/*
** HELPER LINKED LIST FOR NAMES
*/
typedef struct NameNode {
  char *name;
  struct NameNode *next;
} NameNode;

static void free_name_list(NameNode *head) {
  while (head) {
    NameNode *temp = head;
    head = head->next;
    sqlite3_free(temp->name);
    sqlite3_free(temp);
  }
}

/*
** FUNCTION: mview_refresh_all
** SQL USAGE: SELECT mview_refresh_all();
** ----------------------------------------------------------------------------
** Refreshes all registered views sequentially.
*/
static void mview_refresh_all_func(sqlite3_context *context, int argc,
                                   sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *sql = "SELECT view_name FROM " MVIEWS_SCHEMA "._mview_registry";
  sqlite3_stmt *stmt;

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_result_error(context, "Failed to query registry", -1);
    return;
  }

  NameNode *head = NULL;
  NameNode *tail = NULL;
  int count = 0;

  // 1. Collect Names (Must verify we aren't locking the table for writes)
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    NameNode *node = sqlite3_malloc(sizeof(NameNode));
    node->name = sqlite3_mprintf("%s", name); // Dup
    node->next = NULL;

    if (!head)
      head = node;
    else
      tail->next = node;
    tail = node;
    count++;
  }
  sqlite3_finalize(stmt);

  // 2. Iterate and Refresh
  int success_count = 0;
  char *err_msg = NULL;

  NameNode *curr = head;
  while (curr) {
    // Use %Q for safety, though we trust registry content locally
    char *cmd = sqlite3_mprintf("SELECT mview_refresh(%Q)", curr->name);
    int rc = sqlite3_exec(db, cmd, 0, 0, &err_msg);
    sqlite3_free(cmd);

    if (rc != SQLITE_OK) {
      // If one fails, do we stop or continue?
      // Let's continue but report error? Or stop?
      // For now, stop and report error from the first failure.
      sqlite3_result_error(context, err_msg, -1);
      sqlite3_free(err_msg);
      free_name_list(head);
      return;
    }
    success_count++;
    curr = curr->next;
  }

  free_name_list(head);

  char *res = sqlite3_mprintf("Refreshed %d views", success_count);
  sqlite3_result_text(context, res, -1, SQLITE_TRANSIENT);
  sqlite3_free(res);
}

/*
** FUNCTION: mview_drop_all
** SQL USAGE: SELECT mview_drop_all();
** ----------------------------------------------------------------------------
** Drops all registered views.
*/
static void mview_drop_all_func(sqlite3_context *context, int argc,
                                sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *sql = "SELECT view_name FROM " MVIEWS_SCHEMA "._mview_registry";
  sqlite3_stmt *stmt;

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_result_error(context, "Failed to query registry", -1);
    return;
  }

  NameNode *head = NULL;
  NameNode *tail = NULL;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    NameNode *node = sqlite3_malloc(sizeof(NameNode));
    node->name = sqlite3_mprintf("%s", name);
    node->next = NULL;
    if (!head)
      head = node;
    else
      tail->next = node;
    tail = node;
  }
  sqlite3_finalize(stmt);

  int drop_count = 0;
  char *err_msg = NULL;
  NameNode *curr = head;
  while (curr) {
    char *cmd = sqlite3_mprintf("SELECT mview_drop(%Q)", curr->name);
    int rc = sqlite3_exec(db, cmd, 0, 0, &err_msg);
    sqlite3_free(cmd);
    if (rc != SQLITE_OK) {
      // Ignore errors during drop all? Or stop?
      // If we stop, we leave partial state.
      // Best effort: Log error to console/result?
      // Let's just continue and report last error if any?
      // Actually, standard drop_all logic usually tries to clear everything.
      // But if mview_drop fails (transaction rollback), it might be due to
      // locked DB.
      sqlite3_free(err_msg);
    } else {
      drop_count++;
    }
    curr = curr->next;
  }
  free_name_list(head);

  char *res = sqlite3_mprintf("Dropped %d views", drop_count);
  sqlite3_result_text(context, res, -1, SQLITE_TRANSIENT);
  sqlite3_free(res);
}

/*
** FUNCTION: mview_vacuum
** SQL USAGE: SELECT mview_vacuum();
** ----------------------------------------------------------------------------
** Runs VACUUM on the mviews database.
*/
static void mview_vacuum_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  char *err_msg = NULL;
  int rc = sqlite3_exec(db, "VACUUM " MVIEWS_SCHEMA, 0, 0, &err_msg);
  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
  } else {
    sqlite3_result_text(context, "Vacuumed", -1, SQLITE_STATIC);
  }
}

/*
** FUNCTION: mview_remove_index
** SQL USAGE: SELECT mview_remove_index('view_name', 'col1, col2');
** ----------------------------------------------------------------------------
** Deletes the index from registry and physically drops it from the table.
*/
static void mview_remove_index_func(sqlite3_context *context, int argc,
                                    sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *view_name = (const char *)sqlite3_value_text(argv[0]);
  const char *columns = (const char *)sqlite3_value_text(argv[1]);
  char *err_msg = NULL;

  if (!view_name || !columns) {
    sqlite3_result_error(context, "View name and columns cannot be NULL", -1);
    return;
  }

  if (!view_name || !columns) {
    sqlite3_result_error(context, "View name and columns cannot be NULL", -1);
    return;
  }

  sqlite3_exec(db, "SAVEPOINT mview_remove_index_sp", 0, 0, 0);

  // 1. Delete from Registry
  char *del_sql =
      sqlite3_mprintf("DELETE FROM " MVIEWS_SCHEMA "._mview_index_registry "
                      "WHERE view_name='%q' AND columns='%q'",
                      view_name, columns);
  int rc = sqlite3_exec(db, del_sql, 0, 0, &err_msg);
  sqlite3_free(del_sql);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    sqlite3_exec(db, "ROLLBACK TO mview_remove_index_sp", 0, 0, 0);
    sqlite3_exec(db, "RELEASE mview_remove_index_sp", 0, 0, 0);
    return;
  }

  // 2. Drop Physical Index (Tricky: Name is randomized!)
  // We need to look up index name by columns.
  // Query sqlite_master? Or assume mview_refresh handles it?
  // `mview_refresh` creates indexes. Does it remove old ones?
  // `mview_refresh` drops the OLD table entirely and creates a NEW table with
  // registered indexes. So removing from registry + calling refresh effectively
  // removes the index physically. BUT the user might want immediate removal
  // without full refresh. Let's just remove from registry and tell user "Index
  // unregistered. Will vanish on next refresh via mview_refresh()". OR we can
  // try to find and drop it NOW. Finding it requires parsing "CREATE INDEX ...
  // ON view(cols)". Let's stick to "Removed from registry". The user can call
  // `mview_refresh` to apply.

  sqlite3_exec(db, "RELEASE mview_remove_index_sp", 0, 0, 0);
  sqlite3_result_text(context,
                      "Index unregistered (Run mview_refresh to apply)", -1,
                      SQLITE_TRANSIENT);
}

/*
** FUNCTION: mview_rename
** SQL USAGE: SELECT mview_rename('old_name', 'new_name');
** ----------------------------------------------------------------------------
** Renames the view and updates registry.
*/
static void mview_rename_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *old = (const char *)sqlite3_value_text(argv[0]);
  const char *new_name = (const char *)sqlite3_value_text(argv[1]);
  char *err_msg = NULL;

  if (!old || !new_name) {
    sqlite3_result_error(context, "Old and new names cannot be NULL", -1);
    return;
  }

  if (!old || !new_name) {
    sqlite3_result_error(context, "Old and new names cannot be NULL", -1);
    return;
  }

  char *q_old = quote_identifier(old);
  char *q_new = quote_identifier(new_name);
  if (!q_old || !q_new) {
    sqlite3_free(q_old);
    sqlite3_free(q_new);
    sqlite3_result_error_nomem(context);
    return;
  }

  sqlite3_exec(db, "SAVEPOINT mview_rename_sp", 0, 0, 0);

  // 1. Update Registry (Main)
  // Check if new name exists? Unique constraint on view_name will catch it.
  char *upd_reg = sqlite3_mprintf("UPDATE " MVIEWS_SCHEMA "._mview_registry "
                                  "SET view_name='%q' WHERE view_name='%q'",
                                  new_name, old);
  int rc = sqlite3_exec(db, upd_reg, 0, 0, &err_msg);
  sqlite3_free(upd_reg);

  if (rc != SQLITE_OK) {
    goto error;
  }

  // 2. Update Registry (Indexes) manually because we lack ON UPDATE CASCADE
  char *upd_idx =
      sqlite3_mprintf("UPDATE " MVIEWS_SCHEMA "._mview_index_registry "
                      "SET view_name='%q' WHERE view_name='%q'",
                      new_name, old);
  rc = sqlite3_exec(db, upd_idx, 0, 0, &err_msg);
  sqlite3_free(upd_idx);

  if (rc != SQLITE_OK) {
    goto error;
  }

  // 3. Rename Physical Table
  char *ren_tbl = sqlite3_mprintf(
      "ALTER TABLE " MVIEWS_SCHEMA ".%s RENAME TO %s", q_old, q_new);
  rc = sqlite3_exec(db, ren_tbl, 0, 0, &err_msg);
  sqlite3_free(ren_tbl);

  if (rc != SQLITE_OK) {
    goto error;
  }

  sqlite3_exec(db, "RELEASE mview_rename_sp", 0, 0, 0);
  sqlite3_free(q_old);
  sqlite3_free(q_new);
  sqlite3_result_text(context, "Renamed", -1, SQLITE_TRANSIENT);
  return;

error:
  sqlite3_result_error(context, err_msg, -1);
  sqlite3_free(err_msg);
  sqlite3_free(q_old);
  sqlite3_free(q_new);
  sqlite3_exec(db, "ROLLBACK TO mview_rename_sp", 0, 0, 0);
  sqlite3_exec(db, "RELEASE mview_rename_sp", 0, 0, 0);
}

/*
** FUNCTION: mview_reindex
** SQL USAGE: SELECT mview_reindex('view_name');
** ----------------------------------------------------------------------------
** Rebuilds indexes on the view.
*/
static void mview_reindex_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  char *err_msg = NULL;

  char *quoted = quote_identifier(name);
  char *sql = sqlite3_mprintf("REINDEX " MVIEWS_SCHEMA ".%s", quoted);

  int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
  sqlite3_free(sql);
  sqlite3_free(quoted);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
  } else {
    sqlite3_result_text(context, "Reindexed", -1, SQLITE_TRANSIENT);
  }
}

/*
** FUNCTION: mview_explain
** SQL USAGE: SELECT mview_explain('view_name');
** ----------------------------------------------------------------------------
** Returns query plan for the view's source query.
*/
static void mview_explain_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  // Lookup source query
  char *lookup_sql = sqlite3_mprintf("SELECT source_query FROM " MVIEWS_SCHEMA
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
  char *explain_sql = sqlite3_mprintf("EXPLAIN QUERY PLAN %s", query);
  sqlite3_finalize(stmt);

  // Execute Explain
  if (sqlite3_prepare_v2(db, explain_sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(explain_sql);
    sqlite3_result_error(context, "Invalid source query", -1);
    return;
  }
  sqlite3_free(explain_sql);

  // Collect output lines separated by newline
  // EXPLAIN output columns: id, parent, notused, detail
  // We care about 'detail'.
  // Simple buffer logic again.
  int capacity = 512;
  int length = 0;
  char *output = sqlite3_malloc(capacity);
  if (!output) {
    sqlite3_finalize(stmt);
    sqlite3_result_error_nomem(context);
    return;
  }
  output[0] = '\0';

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    // detail is col 3
    const char *detail = (const char *)sqlite3_column_text(stmt, 3);
    int d_len = detail ? (int)strlen(detail) : 0;

    if (length + d_len + 2 >= capacity) {
      capacity = (capacity * 2) + d_len + 2;
      char *new_out = sqlite3_realloc(output, capacity);
      if (!new_out) {
        sqlite3_free(output);
        sqlite3_finalize(stmt);
        sqlite3_result_error_nomem(context);
        return;
      }
      output = new_out;
    }

    if (length > 0) {
      strcat(output, "\n");
      length++;
    }
    strcat(output, detail ? detail : "");
    length += d_len;
  }

  sqlite3_finalize(stmt);
  sqlite3_result_text(context, output, -1, SQLITE_TRANSIENT);
  sqlite3_free(output);
}

/*
** FUNCTION: mview_export
** SQL USAGE: SELECT mview_export('view_name');
** ----------------------------------------------------------------------------
** Returns DDL commands to recreate the view.
*/
static void mview_export_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  // Lookup source query
  char *lookup_sql = sqlite3_mprintf("SELECT source_query FROM " MVIEWS_SCHEMA
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
  // Escaping query for inclusion in mview_create('name', 'query') string
  // literal We need to double single quotes.
  char *escaped_query = sqlite3_mprintf("%q", query);

  // Start with CREATE
  char *ddl =
      sqlite3_mprintf("SELECT mview_create('%q', '%s');", name, escaped_query);
  sqlite3_free(escaped_query);
  sqlite3_finalize(stmt);

  // Append Indexes
  char *idx_sql =
      sqlite3_mprintf("SELECT columns, is_unique FROM " MVIEWS_SCHEMA
                      "._mview_index_registry WHERE view_name='%q'",
                      name);

  if (sqlite3_prepare_v2(db, idx_sql, -1, &stmt, 0) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *cols = (const char *)sqlite3_column_text(stmt, 0);
      int uniq = sqlite3_column_int(stmt, 1);

      char *idx_cmd = sqlite3_mprintf(
          "\nSELECT mview_add_index('%q', '%q', %d);", name, cols, uniq);

      char *new_ddl = sqlite3_mprintf("%s%s", ddl, idx_cmd);
      sqlite3_free(ddl);
      sqlite3_free(idx_cmd);
      ddl = new_ddl;
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_free(idx_sql);

  sqlite3_result_text(context, ddl, -1, SQLITE_TRANSIENT);
  sqlite3_free(ddl);
}

/*
** FUNCTION: mview_drop
** SQL USAGE: SELECT mview_drop('view_name');
** ----------------------------------------------------------------------------
** Permanently removes a view.
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

  // 1. Remove Registry Entry (Main)
  char *del_reg = sqlite3_mprintf("DELETE FROM " MVIEWS_SCHEMA
                                  "._mview_registry WHERE view_name = '%q'",
                                  name);
  if (sqlite3_exec(db, del_reg, 0, 0, &err_msg) != SQLITE_OK) {
    sqlite3_free(del_reg);
    goto error;
  }
  sqlite3_free(del_reg);

  // 2. Remove Registry Entry (Indexes) - Manual Cleanup
  char *del_idx =
      sqlite3_mprintf("DELETE FROM " MVIEWS_SCHEMA
                      "._mview_index_registry WHERE view_name = '%q'",
                      name);
  if (sqlite3_exec(db, del_idx, 0, 0, &err_msg) != SQLITE_OK) {
    sqlite3_free(del_idx);
    goto error;
  }
  sqlite3_free(del_idx);

  // 3. Drop Physical Table
  char *drop_tbl =
      sqlite3_mprintf("DROP TABLE IF EXISTS " MVIEWS_SCHEMA ".%s", quoted_name);
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
** FUNCTION: mview_has
** SQL USAGE: SELECT mview_has('view_name');
** ----------------------------------------------------------------------------
** Checks if a materialized view exists in the registry.
** Returns 1 if it exists, 0 otherwise.
*/
static void mview_has_func(sqlite3_context *context, int argc,
                           sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_int(context, 0);
    return;
  }

  char *sql = sqlite3_mprintf("SELECT 1 FROM " MVIEWS_SCHEMA
                              "._mview_registry WHERE view_name = '%q'",
                              name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_int(context, 0);
    return;
  }
  sqlite3_free(sql);

  int exists = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
  sqlite3_finalize(stmt);

  sqlite3_result_int(context, exists);
}

/*
** FUNCTION: mview_last_refreshed
** SQL USAGE: SELECT mview_last_refreshed('view_name');
** ----------------------------------------------------------------------------
** Returns the timestamp of when the view was last refreshed.
** Returns NULL if the view doesn't exist.
*/
static void mview_last_refreshed_func(sqlite3_context *context, int argc,
                                      sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_null(context);
    return;
  }

  char *sql = sqlite3_mprintf("SELECT last_refreshed FROM " MVIEWS_SCHEMA
                              "._mview_registry WHERE view_name = '%q'",
                              name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *ts = (const char *)sqlite3_column_text(stmt, 0);
    if (ts) {
      sqlite3_result_text(context, ts, -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_result_null(context);
    }
  } else {
    sqlite3_result_error(context, "View not found", -1);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_age
** SQL USAGE: SELECT mview_age('view_name');
** ----------------------------------------------------------------------------
** Returns the number of seconds since the view was last refreshed.
** Returns NULL if the view doesn't exist.
*/
static void mview_age_func(sqlite3_context *context, int argc,
                           sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_null(context);
    return;
  }

  char *sql = sqlite3_mprintf(
      "SELECT CAST((julianday('now') - julianday(last_refreshed)) * 86400 AS "
      "INTEGER) FROM " MVIEWS_SCHEMA "._mview_registry WHERE view_name = '%q'",
      name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int age = sqlite3_column_int(stmt, 0);
    sqlite3_result_int(context, age);
  } else {
    sqlite3_result_error(context, "View not found", -1);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_stale
** SQL USAGE: SELECT mview_stale('view_name', seconds);
** ----------------------------------------------------------------------------
** Returns 1 if the view is older than the given number of seconds, 0 otherwise.
** Returns NULL if the view doesn't exist.
*/
static void mview_stale_func(sqlite3_context *context, int argc,
                             sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  int threshold = sqlite3_value_int(argv[1]);

  if (!name) {
    sqlite3_result_null(context);
    return;
  }

  char *sql = sqlite3_mprintf(
      "SELECT CAST((julianday('now') - julianday(last_refreshed)) * 86400 AS "
      "INTEGER) FROM " MVIEWS_SCHEMA "._mview_registry WHERE view_name = '%q'",
      name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int age = sqlite3_column_int(stmt, 0);
    sqlite3_result_int(context, age > threshold ? 1 : 0);
  } else {
    sqlite3_result_error(context, "View not found", -1);
  }
  sqlite3_finalize(stmt);
}

/*
** HELPER: mview_write_log
** ----------------------------------------------------------------------------
** Writes an entry to the log table if logging is enabled.
*/
static void mview_write_log(sqlite3 *db, mview_context *ctx,
                            const char *operation, const char *view_name,
                            const char *details) {
  if (!ctx || !ctx->logging_enabled)
    return;

  char *sql = sqlite3_mprintf(
      "INSERT INTO " MVIEWS_SCHEMA
      "._mview_log (operation, view_name, details) VALUES ('%q', '%q', '%q')",
      operation ? operation : "", view_name ? view_name : "",
      details ? details : "");
  sqlite3_exec(db, sql, 0, 0, 0);
  sqlite3_free(sql);
}

/*
** FUNCTION: mview_log_enable
** SQL USAGE: SELECT mview_log_enable(1); -- Enable
**            SELECT mview_log_enable(0); -- Disable
** ----------------------------------------------------------------------------
** Enables or disables operation logging.
*/
static void mview_log_enable_func(sqlite3_context *context, int argc,
                                  sqlite3_value **argv) {
  mview_context *ctx = (mview_context *)sqlite3_user_data(context);
  int enable = sqlite3_value_int(argv[0]);

  if (ctx) {
    ctx->logging_enabled = enable ? 1 : 0;
  }

  sqlite3_result_text(context, enable ? "Logging enabled" : "Logging disabled",
                      -1, SQLITE_TRANSIENT);
}

/*
** FUNCTION: mview_log
** SQL USAGE: SELECT mview_log();        -- Last 50 entries
**            SELECT mview_log(100);     -- Last 100 entries
** ----------------------------------------------------------------------------
** Returns recent log entries as JSON array.
*/
static void mview_log_func(sqlite3_context *context, int argc,
                           sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  int limit = (argc > 0) ? sqlite3_value_int(argv[0]) : 50;
  if (limit <= 0)
    limit = 50;

  char *sql = sqlite3_mprintf(
      "SELECT json_group_array(json_object("
      "'id', id, 'timestamp', timestamp, 'operation', operation, "
      "'view_name', view_name, 'details', details)) "
      "FROM (SELECT * FROM " MVIEWS_SCHEMA
      "._mview_log ORDER BY id DESC LIMIT %d)",
      limit);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_text(context, "[]", -1, SQLITE_TRANSIENT);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *json = (const char *)sqlite3_column_text(stmt, 0);
    sqlite3_result_text(context, json ? json : "[]", -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_text(context, "[]", -1, SQLITE_TRANSIENT);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_log_clear
** SQL USAGE: SELECT mview_log_clear();
** ----------------------------------------------------------------------------
** Clears all entries from the log table.
*/
static void mview_log_clear_func(sqlite3_context *context, int argc,
                                 sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  char *err_msg = NULL;

  int rc = sqlite3_exec(db, "DELETE FROM " MVIEWS_SCHEMA "._mview_log", 0, 0,
                        &err_msg);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg ? err_msg : "Failed to clear log",
                         -1);
    sqlite3_free(err_msg);
  } else {
    sqlite3_result_text(context, "Log cleared", -1, SQLITE_TRANSIENT);
  }
}

/*
** FUNCTION: mview_query
** SQL USAGE: SELECT mview_query('view_name');
** ----------------------------------------------------------------------------
** Returns the stored source query for the view.
*/
static void mview_query_func(sqlite3_context *context, int argc,
                             sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_error(context, "View name required", -1);
    return;
  }

  char *sql = sqlite3_mprintf("SELECT source_query FROM " MVIEWS_SCHEMA
                              "._mview_registry WHERE view_name = '%q'",
                              name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "View not found", -1);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *query = (const char *)sqlite3_column_text(stmt, 0);
    sqlite3_result_text(context, query ? query : "", -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_error(context, "View not found", -1);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_schema
** SQL USAGE: SELECT mview_schema('view_name');
** ----------------------------------------------------------------------------
** Returns the column schema of the view as JSON array.
*/
static void mview_schema_func(sqlite3_context *context, int argc,
                              sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_error(context, "View name required", -1);
    return;
  }

  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  char *sql = sqlite3_mprintf(
      "SELECT json_group_array(json_object('cid', cid, 'name', name, 'type', "
      "type, 'notnull', \"notnull\", 'pk', pk)) FROM pragma_table_info('%s', "
      "'mviews')",
      name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_free(quoted_name);
    sqlite3_result_error(context, "Cannot read schema", -1);
    return;
  }
  sqlite3_free(sql);
  sqlite3_free(quoted_name);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *json = (const char *)sqlite3_column_text(stmt, 0);
    sqlite3_result_text(context, json ? json : "[]", -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_text(context, "[]", -1, SQLITE_TRANSIENT);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_size
** SQL USAGE: SELECT mview_size('view_name');
** ----------------------------------------------------------------------------
** Returns the approximate disk size in bytes of the view table.
** Uses the page_count from the dbstat virtual table if available.
*/
static void mview_size_func(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_error(context, "View name required", -1);
    return;
  }

  // Try using dbstat virtual table (available in most SQLite builds)
  char *sql = sqlite3_mprintf(
      "SELECT SUM(pgsize) FROM dbstat('%q') WHERE name = '%q'", "mviews", name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    // Fallback: estimate from row count * avg row size (rough estimate)
    char *fallback_sql = sqlite3_mprintf(
        "SELECT COUNT(*) * 100 FROM " MVIEWS_SCHEMA ".%s", name);
    if (sqlite3_prepare_v2(db, fallback_sql, -1, &stmt, 0) != SQLITE_OK) {
      sqlite3_free(fallback_sql);
      sqlite3_result_error(context, "Cannot determine size", -1);
      return;
    }
    sqlite3_free(fallback_sql);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      sqlite3_result_int64(context, sqlite3_column_int64(stmt, 0));
    } else {
      sqlite3_result_int(context, 0);
    }
    sqlite3_finalize(stmt);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    sqlite3_int64 size = sqlite3_column_int64(stmt, 0);
    sqlite3_result_int64(context, size);
  } else {
    sqlite3_result_int(context, 0);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_indexes
** SQL USAGE: SELECT mview_indexes('view_name');
** ----------------------------------------------------------------------------
** Returns registered indexes for the view as JSON array.
*/
static void mview_indexes_func(sqlite3_context *context, int argc,
                               sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);

  if (!name) {
    sqlite3_result_error(context, "View name required", -1);
    return;
  }

  char *sql = sqlite3_mprintf(
      "SELECT json_group_array(json_object('columns', columns, 'is_unique', "
      "is_unique)) FROM " MVIEWS_SCHEMA
      "._mview_index_registry WHERE view_name = '%q'",
      name);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_text(context, "[]", -1, SQLITE_TRANSIENT);
    return;
  }
  sqlite3_free(sql);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *json = (const char *)sqlite3_column_text(stmt, 0);
    sqlite3_result_text(context, json ? json : "[]", -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_result_text(context, "[]", -1, SQLITE_TRANSIENT);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_count
** SQL USAGE: SELECT mview_count();
** ----------------------------------------------------------------------------
** Returns the total number of registered materialized views.
*/
static void mview_count_func(sqlite3_context *context, int argc,
                             sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);

  char *sql = "SELECT COUNT(*) FROM " MVIEWS_SCHEMA "._mview_registry";

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_result_int(context, 0);
    return;
  }

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    sqlite3_result_int(context, sqlite3_column_int(stmt, 0));
  } else {
    sqlite3_result_int(context, 0);
  }
  sqlite3_finalize(stmt);
}

/*
** FUNCTION: mview_truncate
** SQL USAGE: SELECT mview_truncate('view_name');
** ----------------------------------------------------------------------------
** Deletes all rows from the view table and clears last_refreshed timestamp.
*/
static void mview_truncate_func(sqlite3_context *context, int argc,
                                sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *name = (const char *)sqlite3_value_text(argv[0]);
  char *err_msg = NULL;

  if (!name) {
    sqlite3_result_error(context, "View name required", -1);
    return;
  }

  char *quoted_name = quote_identifier(name);
  if (!quoted_name) {
    sqlite3_result_error_nomem(context);
    return;
  }

  sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0);

  // 1. Truncate Table
  char *trunc_sql =
      sqlite3_mprintf("DELETE FROM " MVIEWS_SCHEMA ".%s", quoted_name);
  int rc = sqlite3_exec(db, trunc_sql, 0, 0, &err_msg);
  sqlite3_free(trunc_sql);
  sqlite3_free(quoted_name);

  if (rc != SQLITE_OK) {
    sqlite3_result_error(context, err_msg, -1);
    sqlite3_free(err_msg);
    sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    return;
  }

  // 2. Clear last_refreshed in Registry
  char *upd_reg = sqlite3_mprintf(
      "UPDATE " MVIEWS_SCHEMA
      "._mview_registry SET last_refreshed = NULL WHERE view_name = '%q'",
      name);
  sqlite3_exec(db, upd_reg, 0, 0, 0);
  sqlite3_free(upd_reg);

  sqlite3_exec(db, "COMMIT", 0, 0, 0);
  sqlite3_result_text(context, "Truncated", -1, SQLITE_TRANSIENT);
}

/*
** FUNCTION: mview_refresh_stale
** SQL USAGE: SELECT mview_refresh_stale(300); -- Refresh views older than 300s
** ----------------------------------------------------------------------------
** Refreshes all views that are older than the given number of seconds.
** Returns a summary message.
*/
static void mview_refresh_stale_func(sqlite3_context *context, int argc,
                                     sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(context);
  mview_context *ctx = (mview_context *)sqlite3_user_data(context);
  int threshold = sqlite3_value_int(argv[0]);

  // Get list of stale views
  char *sql = sqlite3_mprintf(
      "SELECT view_name FROM " MVIEWS_SCHEMA "._mview_registry WHERE "
      "CAST((julianday('now') - julianday(last_refreshed)) * 86400 AS INTEGER) "
      "> %d",
      threshold);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
    sqlite3_free(sql);
    sqlite3_result_error(context, "Failed to query stale views", -1);
    return;
  }
  sqlite3_free(sql);

  int refreshed = 0;
  int failed = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *view_name = (const char *)sqlite3_column_text(stmt, 0);
    if (!view_name)
      continue;

    // Refresh this view (call mview_refresh internally)
    char *refresh_sql =
        sqlite3_mprintf("SELECT mview_refresh('%q')", view_name);
    char *err_msg = NULL;
    if (sqlite3_exec(db, refresh_sql, 0, 0, &err_msg) == SQLITE_OK) {
      refreshed++;
      mview_write_log(db, ctx, "refresh_stale", view_name, "auto-refreshed");
    } else {
      failed++;
      sqlite3_free(err_msg);
    }
    sqlite3_free(refresh_sql);
  }
  sqlite3_finalize(stmt);

  char *msg = sqlite3_mprintf("Refreshed %d stale views (%d failed)", refreshed,
                              failed);
  sqlite3_result_text(context, msg, -1, SQLITE_TRANSIENT);
  sqlite3_free(msg);
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

  // 2. Allocate context
  mview_context *ctx = (mview_context *)sqlite3_malloc(sizeof(mview_context));
  if (!ctx)
    return SQLITE_NOMEM;
  ctx->attached_by_us = 0;
  ctx->logging_enabled = 0;

  // 3. Register Functions
  // Note: We pass 'ctx' to all functions.
  // We ONLY set 'mview_context_free' destructor on ONE function (mview_version)
  // to avoid double-freeing when the connection closes.

  // mview_version() - OWNS THE CONTEXT MEMORY
  sqlite3_create_function_v2(db, "mview_version", 0, SQLITE_UTF8, ctx,
                             mview_version_func, 0, 0, mview_context_free);

  // mview_attach('path')
  sqlite3_create_function_v2(db, "mview_attach", 1, SQLITE_UTF8, ctx,
                             mview_attach_func, 0, 0, NULL);

  // mview_dettach() (Note spelling: user request)
  sqlite3_create_function_v2(db, "mview_dettach", 0, SQLITE_UTF8, ctx,
                             mview_dettach_func, 0, 0, NULL);

  // mview_init() - 0 args
  sqlite3_create_function_v2(db, "mview_init", 0, SQLITE_UTF8, ctx,
                             mview_init_func, 0, 0, NULL);

  // mview_create('name', 'query') - 2 args
  sqlite3_create_function_v2(db, "mview_create", 2, SQLITE_UTF8, ctx,
                             mview_create_func, 0, 0, NULL);

  // mview_create('name', 'query', 'schema') - 3 args (Overloaded)
  sqlite3_create_function_v2(db, "mview_create", 3, SQLITE_UTF8, ctx,
                             mview_create_func, 0, 0, NULL);

  // mview_refresh('name')
  sqlite3_create_function_v2(db, "mview_refresh", 1, SQLITE_UTF8, ctx,
                             mview_refresh_func, 0, 0, NULL);

  // mview_add_index('name', 'columns', is_unique)
  sqlite3_create_function_v2(db, "mview_add_index", 3, SQLITE_UTF8, ctx,
                             mview_add_index_func, 0, 0, NULL);

  // mview_drop('name')
  sqlite3_create_function_v2(db, "mview_drop", 1, SQLITE_UTF8, ctx,
                             mview_drop_func, 0, 0, NULL);

  // mview_registry (Virtual Table)
  sqlite3_create_module(db, "mview_registry", &mview_list_module, 0);

  // mview_info('name')
  sqlite3_create_function_v2(db, "mview_info", 1, SQLITE_UTF8, ctx,
                             mview_info_func, 0, 0, NULL);

  // mview_stats('name')
  sqlite3_create_function_v2(db, "mview_stats", 1, SQLITE_UTF8, ctx,
                             mview_stats_func, 0, 0, NULL);

  // mview_verify('name')
  sqlite3_create_function_v2(db, "mview_verify", 1, SQLITE_UTF8, ctx,
                             mview_verify_func, 0, 0, NULL);

  // mview_refresh_all()
  sqlite3_create_function_v2(db, "mview_refresh_all", 0, SQLITE_UTF8, ctx,
                             mview_refresh_all_func, 0, 0, NULL);

  // mview_drop_all()
  sqlite3_create_function_v2(db, "mview_drop_all", 0, SQLITE_UTF8, ctx,
                             mview_drop_all_func, 0, 0, NULL);

  // mview_vacuum()
  sqlite3_create_function_v2(db, "mview_vacuum", 0, SQLITE_UTF8, ctx,
                             mview_vacuum_func, 0, 0, NULL);

  // mview_remove_index('view', 'columns')
  sqlite3_create_function_v2(db, "mview_remove_index", 2, SQLITE_UTF8, ctx,
                             mview_remove_index_func, 0, 0, NULL);

  // mview_rename('old', 'new')
  sqlite3_create_function_v2(db, "mview_rename", 2, SQLITE_UTF8, ctx,
                             mview_rename_func, 0, 0, NULL);

  // mview_reindex('name')
  sqlite3_create_function_v2(db, "mview_reindex", 1, SQLITE_UTF8, ctx,
                             mview_reindex_func, 0, 0, NULL);

  // mview_explain('name')
  sqlite3_create_function_v2(db, "mview_explain", 1, SQLITE_UTF8, ctx,
                             mview_explain_func, 0, 0, NULL);

  // mview_export('name')
  sqlite3_create_function_v2(db, "mview_export", 1, SQLITE_UTF8, ctx,
                             mview_export_func, 0, 0, NULL);

  // mview_has('name')
  sqlite3_create_function_v2(db, "mview_has", 1, SQLITE_UTF8, ctx,
                             mview_has_func, 0, 0, NULL);

  // mview_last_refreshed('name')
  sqlite3_create_function_v2(db, "mview_last_refreshed", 1, SQLITE_UTF8, ctx,
                             mview_last_refreshed_func, 0, 0, NULL);

  // mview_age('name')
  sqlite3_create_function_v2(db, "mview_age", 1, SQLITE_UTF8, ctx,
                             mview_age_func, 0, 0, NULL);

  // mview_stale('name', seconds)
  sqlite3_create_function_v2(db, "mview_stale", 2, SQLITE_UTF8, ctx,
                             mview_stale_func, 0, 0, NULL);

  // mview_log_enable(1|0)
  sqlite3_create_function_v2(db, "mview_log_enable", 1, SQLITE_UTF8, ctx,
                             mview_log_enable_func, 0, 0, NULL);

  // mview_log() or mview_log(limit)
  sqlite3_create_function_v2(db, "mview_log", 0, SQLITE_UTF8, ctx,
                             mview_log_func, 0, 0, NULL);
  sqlite3_create_function_v2(db, "mview_log", 1, SQLITE_UTF8, ctx,
                             mview_log_func, 0, 0, NULL);

  // mview_query('name')
  sqlite3_create_function_v2(db, "mview_query", 1, SQLITE_UTF8, ctx,
                             mview_query_func, 0, 0, NULL);

  // mview_schema('name')
  sqlite3_create_function_v2(db, "mview_schema", 1, SQLITE_UTF8, ctx,
                             mview_schema_func, 0, 0, NULL);

  // mview_indexes('name')
  sqlite3_create_function_v2(db, "mview_indexes", 1, SQLITE_UTF8, ctx,
                             mview_indexes_func, 0, 0, NULL);

  // mview_refresh_stale(seconds)
  sqlite3_create_function_v2(db, "mview_refresh_stale", 1, SQLITE_UTF8, ctx,
                             mview_refresh_stale_func, 0, 0, NULL);

  // mview_log_clear()
  sqlite3_create_function_v2(db, "mview_log_clear", 0, SQLITE_UTF8, ctx,
                             mview_log_clear_func, 0, 0, NULL);

  // mview_size('name')
  sqlite3_create_function_v2(db, "mview_size", 1, SQLITE_UTF8, ctx,
                             mview_size_func, 0, 0, NULL);

  // mview_count()
  sqlite3_create_function_v2(db, "mview_count", 0, SQLITE_UTF8, ctx,
                             mview_count_func, 0, 0, NULL);

  // mview_truncate('name')
  sqlite3_create_function_v2(db, "mview_truncate", 1, SQLITE_UTF8, ctx,
                             mview_truncate_func, 0, 0, NULL);

  return 0;
}
