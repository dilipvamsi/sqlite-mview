# SQLite Memorized Views (sqlite-mview)

![License](https://img.shields.io/badge/license-MIT-blue.svg) ![Platform](https://img.shields.io/badge/platform-sqlite-green.svg) ![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

A robust, persistent, and **concurrent** Materialized View extension for SQLite.

While standard SQLite views are virtual (re-calculated every time you query them), **sqlite-mview** "memorizes" the results of complex queries into real physical tables.

### ⚡ Key Benefits
*   **Instant Reads:** No matter how complex the source query (Joins, Aggregates, Window Functions), reading the view is O(1).
*   **Non-Blocking Refreshes:** Uses a **Shadow Paging** strategy. Readers can continue to read old data while the new view is being calculated in the background.
*   **Zero Main-DB Bloat:** All view data is stored in a separate, attached database file.
*   **Persistent Indexes:** Indexes are automatically managed and re-applied after every refresh.

---

## 🏗️ Architecture

This extension uses the **"Sidecar Database" pattern**. It attaches a secondary database file to your connection to store the cache.

```text
+-------------+        +--------------------------+
| Application | -----> |  Main Database (source)  |
+-------------+        +--------------------------+
       |
       |  (Attached as 'mviews')
       v
+--------------------------+
|  Cache Database (mviews) |
| ------------------------ |
|  [ table: daily_stats ]  | <--- Physical Table
|  [ table: user_counts ]  |
+--------------------------+
```

---

## 🚀 Features

*   **Shadow Paging Strategy:** Refreshes are performed by creating a temporary table, populating it, indexing it, and then atomically swapping it with the live table.
*   **High Concurrency:** The refresh operation is **non-blocking** for readers.
*   **Strict Typing:** Option to define Primary Keys and strict column types.
*   **WAL Mode:** Automatically enables Write-Ahead Logging on the cache DB.
*   **Index Registry:** Maintains a registry of indexes and automatically re-builds them whenever the view is refreshed.

---

## 🛠️ Installation & Building

### Prerequisites
*   GCC or Clang compiler.
*   `sqlite3ext.h` (Included in `libsqlite3-dev` on Linux).

### Build
Run the `make` command. It automatically detects your OS.

```bash
make
```
*   **Linux:** `build/mview.so`
*   **macOS:** `build/mview.dylib`
*   **Windows:** `build/mview.dll`

> **Troubleshooting:** If the build fails with `fatal error: sqlite3ext.h: No such file`, you need to install SQLite development headers (e.g., `sudo apt install libsqlite3-dev`) or download the amalgamation zip from sqlite.org and place `sqlite3ext.h` in this folder.

---

## 📖 Usage Guide

### 1. Load the Extension
Start `sqlite3` (or your app) and load the binary.

```sql
.load ./build/mview
```

### 2. Initialize the Storage
Initialize the "cache" database.

```sql
-- Creates (or opens) 'cache.db' and attaches it as 'mviews'
SELECT mview_init('cache.db');
```

### 3. Create a View (Simple Mode)
SQLite automatically detects column types based on the result.

```sql
SELECT mview_create(
    'daily_stats',
    'SELECT date, sum(total) as revenue FROM main.orders GROUP BY date'
);
```

> **💡 Best Practice:** Always prefix your source tables with `main.` (e.g., `SELECT * FROM main.orders`). This guarantees your view reads from your actual database, preventing conflicts if a temporary table with the same name exists.

### 4. Create a View (Strict Mode)
Use this to define a **Primary Key** or specific types.

```sql
SELECT mview_create(
    'users_summary',
    -- Source Query
    'SELECT id, username, count(*) FROM main.users JOIN main.posts ON u.id = p.user_id GROUP BY u.id',
    -- Schema Definition
    'id INTEGER PRIMARY KEY, username TEXT, post_count INTEGER'
);
```

### 5. Add Indexes (Crucial!)
Because the table is dropped and re-created during a refresh, you **cannot** use standard `CREATE INDEX` SQL, as the index would disappear after the next refresh.
Use `mview_add_index` instead:

```sql
-- Format: mview_add_index(view_name, columns, is_unique_boolean)

-- Create a standard index on 'revenue'
SELECT mview_add_index('daily_stats', 'revenue', 0);

-- Create a UNIQUE index on 'date'
SELECT mview_add_index('daily_stats', 'date', 1);
```

### 6. Refresh the Data
Trigger a refresh when your main data changes.

```sql
SELECT mview_refresh('daily_stats');
```
*This performs the following atomic steps:*
1. Creates a temporary table (e.g., `daily_stats_new_a1b2`).
2. Populates it from the source query (Readers continue reading the old table).
3. Applies registered indexes to the temporary table.
4. Swaps the tables inside a transaction.

### 7. Query the Data
The views exist in the `mviews` schema.

```sql
-- Fast read from physical table
SELECT * FROM mviews.daily_stats WHERE revenue > 1000;
```

### 8. Drop a View
Permanently remove a view, its indexes, and its metadata.

```sql
SELECT mview_drop('daily_stats');
```

### 9. Clean Up
Detaches the cache database. Data persists on disk.

```sql
SELECT mview_close();
```

---

## ⚠️ Limitations

1.  **Full Refresh Only:** This extension performs a complete recalculation. It does not support "Incremental Updates" (delta updates). It is best suited for complex read-heavy queries rather than massive write-heavy tables.
2.  **Attached DBs:** SQLite has a limit on attached databases (usually 10 or 30). This extension uses one slot.
3.  **Space Usage:** During a refresh, disk usage for that specific view temporarily doubles (Old Table + New Table) until the swap is complete.

---

## 🧪 Testing

This project includes a Python-based test suite.

```bash
make test
```

The test suite covers:
*   Creation, Querying, and Dropping.
*   **Shadow Swap Verification:** Ensuring indexes survive refreshes.
*   **Unique Constraints:** Ensuring refreshes fail safely if unique constraints are violated.
*   **Concurrency:** Ensuring initialization is idempotent.

---

## License

MIT License.
