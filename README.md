# SQLite Memorized Views (sqlite-mview)

![License](https://img.shields.io/badge/license-MIT-blue.svg) ![Platform](https://img.shields.io/badge/platform-sqlite-green.svg) ![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

A robust, persistent **Materialized View** extension for SQLite.

While standard SQLite views are virtual (re-calculated every time you query them), **sqlite-mview** "memorizes" the results of complex queries into real physical tables.

### ⚡ Key Benefits
*   **Instant Reads:** No matter how complex the source query (Joins, Aggregates, Window Functions), reading the view is O(1).
*   **Indexing:** You can add standard SQLite indexes to the view for O(log n) lookups.
*   **Zero Main-DB Bloat:** All view data is stored in a separate, attached database file.

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

*   **Manager Approach:** No manual `CREATE TABLE` or `TRIGGER` logic required.
*   **Strict Typing:** Option to define Primary Keys and strict column types.
*   **Atomic Refreshes:** Refreshes use transactions (`DELETE` -> `INSERT`). The view is never empty during a refresh.
*   **WAL Mode:** Automatically enables Write-Ahead Logging on the cache DB for high concurrency.

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

### 4. Create a View (Strict Mode)
Use this to define a **Primary Key** for performance.

```sql
SELECT mview_create(
    'users_summary',
    -- Source Query
    'SELECT id, username, count(*) FROM main.users JOIN main.posts ON u.id = p.user_id GROUP BY u.id',
    -- Schema Definition
    'id INTEGER PRIMARY KEY, username TEXT, post_count INTEGER'
);
```

### 5. Query the Data
The views exist in the `mviews` schema.

```sql
-- Fast read from physical table
SELECT * FROM mviews.daily_stats WHERE revenue > 1000;
```

### 6. Refresh the Data
Trigger a refresh when your main data changes. This performs a full re-calculation inside a transaction.

```sql
SELECT mview_refresh('daily_stats');
```

### 7. Drop a View
Permanently remove a view and its metadata from the registry.

```sql
SELECT mview_drop('daily_stats');
```

### 8. Clean Up
Detaches the cache database. Data persists on disk.

```sql
SELECT mview_close();
```

---

## 🤖 Advanced: Automating Refreshes

You can use standard SQLite Triggers to refresh views automatically.

```sql
CREATE TRIGGER auto_refresh_sales
AFTER INSERT ON main.orders
BEGIN
    -- Automatically refresh the view whenever a new order comes in
    SELECT mview_refresh('daily_stats');
END;
```

---

## ⚠️ Limitations

1.  **Full Refresh Only:** This extension performs a complete recalculation (Truncate & Insert) on refresh. It does not support "Incremental Updates" (delta updates). It is best suited for complex read-heavy queries rather than massive write-heavy tables.
2.  **Attached DBs:** SQLite has a limit on attached databases (usually 10 or 30). This extension uses one slot.

---

## 🧪 Testing

This project includes a Python-based test suite.

```bash
make test
```

The test suite covers:
*   Creating/Querying/Dropping views.
*   Data persistence across sessions.
*   Refresh atomicity (rollback on failure).
*   SQL Injection safety.

---

## License

MIT License.
