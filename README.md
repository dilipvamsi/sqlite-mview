# SQLite Memorized Views (sqlite-mview) v3.0

![License](https://img.shields.io/badge/license-MIT-blue.svg) ![Platform](https://img.shields.io/badge/platform-sqlite-green.svg) ![Coverage](https://img.shields.io/badge/coverage-83%25-brightgreen.svg)

A robust, persistent, and **concurrent** Materialized View extension for SQLite.

While standard SQLite views are virtual (re-calculated every time you query them), **sqlite-mview** "memorizes" the results of complex queries into real physical tables.

### ⚡ Key Benefits
*   **Instant Reads:** No matter how complex the source query, reading the view is O(1).
*   **Non-Blocking Refreshes:** Uses **Shadow Paging** - readers continue reading old data during refresh.
*   **Zero Main-DB Bloat:** All view data is stored in a separate, attached database file.
*   **Persistent Indexes:** Indexes are automatically re-applied after every refresh.

---

## 🏗️ Architecture

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
|  [ _mview_registry ]     | <--- Metadata
|  [ daily_stats ]         | <--- Physical Table
|  [ user_counts ]         |
+--------------------------+
```

---

## 🛠️ Installation

```bash
make                 # Build: Linux=mview.so, macOS=mview.dylib, Windows=mview.dll
make test            # Run tests
make coverage        # Generate coverage report (83%+)
make leak-check      # Valgrind memory check
```

---

## 📖 Quick Start

```sql
-- 1. Load extension
.load ./build/mview

-- 2. Initialize storage (2-step process in v3.0)
SELECT mview_attach('cache.db');   -- Attach file as 'mviews'
SELECT mview_init();               -- Create registry tables

-- 3. Create views
SELECT mview_create('daily_stats',
    'SELECT date, sum(amount) as total FROM main.orders GROUP BY date');

-- With explicit schema (for PRIMARY KEYs)
SELECT mview_create('users_mv', 'SELECT id, name FROM main.users',
    'id INTEGER PRIMARY KEY, name TEXT');

-- 4. Add indexes (survive refreshes)
SELECT mview_add_index('daily_stats', 'date', 1);   -- 1 = UNIQUE
SELECT mview_add_index('daily_stats', 'total', 0);  -- 0 = Non-unique

-- 5. Query (fast!)
SELECT * FROM mviews.daily_stats WHERE total > 1000;

-- 6. Refresh when data changes
SELECT mview_refresh('daily_stats');
SELECT mview_refresh_all();           -- Refresh all views
SELECT mview_refresh_stale(3600);     -- Refresh views older than 1 hour

-- 7. Introspection
SELECT * FROM mview_registry;              -- List all views
SELECT mview_has('daily_stats');           -- Check if exists (1|0)
SELECT mview_query('daily_stats');         -- Get source query
SELECT mview_schema('daily_stats');        -- Column schema (JSON)
SELECT mview_indexes('daily_stats');       -- Registered indexes (JSON)
SELECT mview_info('daily_stats');          -- Full metadata (JSON)
SELECT mview_stats('daily_stats');         -- Row count & size
SELECT mview_verify('daily_stats');        -- Schema consistency check
SELECT mview_explain('daily_stats');       -- Show source query
SELECT mview_export('daily_stats');        -- Export as SQL

-- 8. Time-based cache management
SELECT mview_last_refreshed('daily_stats'); -- Timestamp of last refresh
SELECT mview_age('daily_stats');            -- Seconds since refresh
SELECT mview_stale('daily_stats', 3600);    -- 1 if older than 1 hour

-- 9. Index management
SELECT mview_remove_index('daily_stats', 'total');
SELECT mview_reindex('daily_stats');       -- Rebuild indexes

-- 10. Logging
SELECT mview_log_enable(1);    -- Enable logging
SELECT mview_log();            -- View last 50 log entries
SELECT mview_log_clear();      -- Clear all logs
SELECT mview_log_enable(0);    -- Disable logging

-- 11. Schema evolution
SELECT mview_rename('daily_stats', 'sales_by_day');

-- 12. Cleanup
SELECT mview_drop('sales_by_day');         -- Drop single view
SELECT mview_drop_all();                   -- Drop all views
SELECT mview_vacuum();                     -- Vacuum cache DB
SELECT mview_dettach();                    -- Detach storage

-- Version check
SELECT mview_version();  -- Returns '3.0.0'
```

## 📚 API Reference

### Core Functions

| Function | Description |
|----------|-------------|
| `mview_version()` | Returns version string ('3.0.0') |
| `mview_attach(path)` | Attach cache database file |
| `mview_init()` | Initialize registry tables |
| `mview_dettach()` | Detach cache database |

### View Management

| Function | Description |
|----------|-------------|
| `mview_create(name, query)` | Create view with auto-schema |
| `mview_create(name, query, schema)` | Create view with explicit schema (e.g., `'id INTEGER PRIMARY KEY, name TEXT'`) |
| `mview_refresh(name)` | Refresh single view (atomic swap) |
| `mview_drop(name)` | Drop view and its metadata |
| `mview_rename(old, new)` | Rename a view |

### Index Management

| Function | Description |
|----------|-------------|
| `mview_add_index(view, cols, unique)` | Register index (unique=1 or 0) |
| `mview_remove_index(view, cols)` | Remove registered index |
| `mview_reindex(view)` | Rebuild all indexes |

### Introspection

| Function | Description |
|----------|-------------|
| `mview_registry` | Virtual table listing all views |
| `mview_has(name)` | Returns 1 if view exists, 0 otherwise |
| `mview_query(name)` | Returns the stored source query |
| `mview_schema(name)` | Returns column schema as JSON |
| `mview_indexes(name)` | Returns registered indexes as JSON |
| `mview_last_refreshed(name)` | Returns timestamp of last refresh |
| `mview_age(name)` | Returns seconds since last refresh |
| `mview_stale(name, secs)` | Returns 1 if older than secs, 0 otherwise |
| `mview_info(name)` | JSON metadata for a view |
| `mview_stats(name)` | Row count and size info |
| `mview_size(name)` | Disk size in bytes |
| `mview_count()` | Total number of registered views |
| `mview_verify(name)` | Schema consistency check |
| `mview_explain(name)` | Show source query |
| `mview_export(name)` | Export view definition as SQL |

### Bulk Operations

| Function | Description |
|----------|-------------|
| `mview_refresh_all()` | Refresh all views |
| `mview_refresh_stale(secs)` | Refresh views older than secs |
| `mview_drop_all()` | Drop all views |
| `mview_truncate(name)` | Clear all data but keep metadata |
| `mview_vacuum()` | Vacuum the cache database |

### Logging

| Function | Description |
|----------|-------------|
| `mview_log_enable(1\|0)` | Enable/disable operation logging |
| `mview_log()` | Returns last 50 log entries as JSON |
| `mview_log(limit)` | Returns last N log entries as JSON |
| `mview_log_clear()` | Clear all log entries |

---

## 💡 Best Practices

1. **Prefix source tables with `main.`** to avoid conflicts:
   ```sql
   SELECT mview_create('stats', 'SELECT * FROM main.orders');
   ```

2. **Use strict schema for PRIMARY KEYs:**
   ```sql
   SELECT mview_create('users', 'SELECT id, name FROM main.users',
       'id INTEGER PRIMARY KEY, name TEXT');
   ```

3. **Register indexes before first refresh** - they're applied automatically on every refresh.

---

## ⚠️ Limitations

*   **Full Refresh Only:** No incremental updates. Best for read-heavy workloads.
*   **Attached DB Limit:** Uses 1 of SQLite's ~10 available attachment slots.
*   **Temporary Space:** During refresh, disk usage doubles briefly.

---

## 🧪 Testing

```bash
make test         # 59 tests
make coverage     # 83%+ line coverage
make leak-check   # Valgrind: 0 leaks
make check        # Run all above
```

---

## License

MIT License.
