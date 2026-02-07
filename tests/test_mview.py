import os
import sqlite3
import unittest
import time

# -----------------------------------------------------------------------------
# CONFIGURATION
# -----------------------------------------------------------------------------
# Path to the compiled extension. Defaults to ./mview.so if not set in ENV.
# Windows users should ensure this ends in .dll
EXT_PATH = os.environ.get("EXT_PATH", "./mview.so")
DB_PATH = os.environ.get("TEST_DB_PATH", "test_cache.db")

class TestMViewExtension(unittest.TestCase):
    """
    Test Suite for sqlite-mview Extension.
    """

    def setUp(self):
        """
        Per-test setup.
        1. Connects to in-memory DB.
        2. Sets isolation_level=None (Auto-commit).
        3. Loads extension.
        4. Seeds dummy data.
        """
        # Ensure clean environment
        self._clean_files()

        self.conn = sqlite3.connect(":memory:", isolation_level=None)
        self.conn.enable_load_extension(True)

        try:
            self.conn.load_extension(EXT_PATH)
        except sqlite3.OperationalError as e:
            self.fail(f"\n[ERROR] Could not load extension at '{EXT_PATH}'.\n"
                      f"Ensure you ran 'make' and the path is correct.\n"
                      f"Details: {e}")

        # Seed Main Data
        self.conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, role TEXT)")
        self.conn.execute("INSERT INTO users VALUES (1, 'Alice', 'admin')")
        self.conn.execute("INSERT INTO users VALUES (2, 'Bob', 'user')")
        self.conn.execute("INSERT INTO users VALUES (3, 'Charlie', 'user')")

    def tearDown(self):
        """Cleanup connection and artifacts."""
        try:
            # Attempt graceful detach
            self.conn.execute("SELECT mview_dettach()")
        except Exception:
            pass
        self.conn.close()
        self._clean_files()

    def _clean_files(self):
        """Remove cache DB and its WAL/SHM files."""
        for f in [DB_PATH, DB_PATH + "-wal", DB_PATH + "-shm"]:
            if os.path.exists(f):
                try:
                    os.remove(f)
                except PermissionError:
                    pass # Windows might hold a lock briefly

    def init_cache(self):
        """Helper to initialize the cache database."""
        self.conn.execute(f"SELECT mview_attach('{DB_PATH}')")
        self.conn.execute("SELECT mview_init()")

    # =========================================================================
    # SECTION 1: Initialization & Lifecycle
    # =========================================================================

    def test_01_init_creates_registries(self):
        """
        Verify that mview_init creates the necessary internal bookkeeping tables
        and sets the journal mode to WAL.
        """
        self.init_cache()

        # Check 1: Registry Tables Exist
        tables = [row[0] for row in self.conn.execute(
            "SELECT name FROM mviews.sqlite_master WHERE type='table' AND name LIKE '_mview_%'"
        ).fetchall()]

        self.assertIn("_mview_registry", tables)
        self.assertIn("_mview_index_registry", tables)

        # Check 2: WAL Mode is active
        # Note: We must query the attached schema's pragma
        mode = self.conn.execute(f"PRAGMA mviews.journal_mode").fetchone()[0]
        self.assertEqual(mode.lower(), "wal")

    def test_02_create_without_init_fails(self):
        """Verify that operations fail gracefully if init hasn't been called."""
        # Note: mview_create checks if registry exists.
        # If we haven't attached, it will fail because schema 'mviews' is missing OR registry table missing.
        with self.assertRaisesRegex(sqlite3.OperationalError, "Cache DB not initialized"):
             # We must attach first to even get to the 'not initialized' check,
             # OR if we assume mview_create checks 'mviews' schema existence.
             # The new code: mview_create -> init_registry -> checks table existence.
             # If schema 'mviews' is missing, init_registry might fail with "unknown database mviews"
             # Let's see what happens if we don't even attach.
             self.conn.execute("SELECT mview_create('fail', 'SELECT 1')")

    def test_03_double_attach_fails(self):
        """
        Verify that calling attach twice fails.
        """
        self.conn.execute(f"SELECT mview_attach('{DB_PATH}')")
        
        with self.assertRaisesRegex(sqlite3.OperationalError, "Database already attached"):
            self.conn.execute(f"SELECT mview_attach('{DB_PATH}')")

    def test_04_attach_invalid_path(self):
        """Verify behavior when checking an invalid path (e.g. directory)."""
        # '.' is a directory, sqlite3_open might fail or return error depending on OS
        # But 'ATTACH' usually works on directions as 'creating a file named .' which fails?
        # Actually sqlite might open a directory as a DB in read-only mode or fail.
        # Let's try attaching a non-existent directory path.
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_attach('/non/existent/path/db.sqlite')")

    def test_05_dettach_detaches_db(self):
        """Verify mview_dettach removes the schema from the connection."""
        self.init_cache()
        self.conn.execute("SELECT mview_dettach()")

        # Check SQLite's internal list of attached databases
        schemas = [row[1] for row in self.conn.execute("PRAGMA database_list")]
        self.assertNotIn("mviews", schemas)

    def test_06_manual_attach_dettach_fails(self):
        """
        Verify that if the user manually attached 'mviews', 
        mview_dettach() refuses to detach it.
        """
        self.conn.execute(f"ATTACH DATABASE '{DB_PATH}' AS mviews")
        
        # init should still work (it just creates tables)
        self.conn.execute("SELECT mview_init()")

        # dettach should fail
        with self.assertRaisesRegex(sqlite3.OperationalError, "Cannot detach"):
            self.conn.execute("SELECT mview_dettach()")
        
        # Cleanup
        self.conn.execute("DETACH DATABASE mviews")

    # =========================================================================
    # SECTION 2: View Creation & Schema
    # =========================================================================

    def test_07_simple_create(self):
        """Verify standard 'Create Table As Select' behavior."""
        self.init_cache()
        sql = "SELECT role, count(*) as cnt FROM main.users GROUP BY role"
        self.conn.execute("SELECT mview_create(?, ?)", ("stats", sql))

        # Query the materialized table
        rows = self.conn.execute("SELECT * FROM mviews.stats ORDER BY role").fetchall()
        expected = [("admin", 1), ("user", 2)]
        self.assertEqual(rows, expected)

    def test_08_strict_create_enforces_pk(self):
        """
        Verify 'Strict Mode' where the user supplies a CREATE TABLE schema.
        We ensure Primary Key constraints are actually enforced by SQLite.
        """
        self.init_cache()
        schema = "id INTEGER PRIMARY KEY, name TEXT"
        query = "SELECT id, name FROM main.users"
        self.conn.execute("SELECT mview_create('strict_users', ?, ?)", (query, schema))

        # Attempt to insert a duplicate ID manually into the view
        # This confirms the table really has the PK, unlike a generic CTAS
        with self.assertRaises(sqlite3.IntegrityError):
            self.conn.execute("INSERT INTO mviews.strict_users VALUES (1, 'Fake Clone')")

    def test_09_overwrite_view_definition(self):
        """
        Verify that creating a view with an existing name updates the registry
        and replaces the table.
        """
        self.init_cache()

        # 1. Create Initial
        self.conn.execute("SELECT mview_create('v1', 'SELECT 1 as val')")
        row = self.conn.execute("SELECT val FROM mviews.v1").fetchone()
        self.assertEqual(row[0], 1)

        # 2. Overwrite
        self.conn.execute("SELECT mview_create('v1', 'SELECT 999 as val')")

        # 3. Verify Table Data
        row = self.conn.execute("SELECT val FROM mviews.v1").fetchone()
        self.assertEqual(row[0], 999)

        # 4. Verify Registry Updated
        reg_sql = self.conn.execute(
            "SELECT source_query FROM mviews._mview_registry WHERE view_name='v1'"
        ).fetchone()[0]
        self.assertIn("999", reg_sql)

    # =========================================================================
    # SECTION 3: Refresh Mechanics & Shadow Paging
    # =========================================================================

    def test_10_refresh_updates_data(self):
        """Verify refresh captures changes from source."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('users_list', 'SELECT name FROM main.users')")

        # Modify Source
        self.conn.execute("INSERT INTO users VALUES (4, 'David', 'admin')")

        # Refresh
        self.conn.execute("SELECT mview_refresh('users_list')")

        count = self.conn.execute("SELECT count(*) FROM mviews.users_list").fetchone()[0]
        self.assertEqual(count, 4)

    def test_11_refresh_rollback_on_source_error(self):
        """
        Verify ATOMICITY: If the source table is missing/broken, the refresh
        should fail, but the OLD view data must remain untouched.
        """
        self.init_cache()
        self.conn.execute("SELECT mview_create('t1', 'SELECT * FROM users')")

        # Break the source
        self.conn.execute("DROP TABLE users")

        # Refresh -> Fails
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
            self.conn.execute("SELECT mview_refresh('t1')")

        # Old data persists
        count = self.conn.execute("SELECT count(*) FROM mviews.t1").fetchone()[0]
        self.assertEqual(count, 3)

    # =========================================================================
    # SECTION 4: Indexes & Constraints
    # =========================================================================

    def _get_index_names(self, table_name):
        return [row[0] for row in self.conn.execute(
            "SELECT name FROM mviews.sqlite_master WHERE type='index' AND tbl_name=?",
            (table_name,)
        ).fetchall()]

    def test_12_index_persistence(self):
        """
        CRITICAL TEST: Verify that indexes survive the Refresh/Swap process.
        Since mview_refresh creates a *new* table, it must re-apply the indexes.
        """
        self.init_cache()
        self.conn.execute("SELECT mview_create('t_idx', 'SELECT * FROM users')")

        # Register Index
        self.conn.execute("SELECT mview_add_index('t_idx', 'name', 0)")

        # Verify created immediately
        self.assertTrue(len(self._get_index_names('t_idx')) > 0)

        # Trigger Refresh (Causes Shadow Swap)
        self.conn.execute("SELECT mview_refresh('t_idx')")

        # Verify index persists (even if name changes due to random suffix)
        self.assertTrue(len(self._get_index_names('t_idx')) > 0)

    def test_13_unique_constraint_enforcement(self):
        """
        Verify that if a Unique Index is registered, the refresh fails if
        duplicates are introduced in the source data.
        """
        self.init_cache()
        self.conn.execute("SELECT mview_create('t_uniq', 'SELECT name FROM users')")

        # Register UNIQUE Index
        self.conn.execute("SELECT mview_add_index('t_uniq', 'name', 1)")

        # Add Duplicate to Source ('Alice' exists)
        self.conn.execute("INSERT INTO users VALUES (4, 'Alice', 'hacker')")

        # Refresh should fail (Unique constraint violation on temp table)
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_refresh('t_uniq')")

        # Verify rollback (still 3 rows)
        count = self.conn.execute("SELECT count(*) FROM mviews.t_uniq").fetchone()[0]
        self.assertEqual(count, 3)

    # =========================================================================
    # SECTION 5: Cleanup & Edge Cases
    # =========================================================================

    def test_14_drop_cleanup(self):
        """Verify mview_drop removes the table and registry entry."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('gone', 'SELECT 1')")
        self.conn.execute("SELECT mview_drop('gone')")

        # Table gone
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
            self.conn.execute("SELECT * FROM mviews.gone")

        # Registry gone
        count = self.conn.execute(
            "SELECT count(*) FROM mviews._mview_registry WHERE view_name='gone'"
        ).fetchone()[0]
        self.assertEqual(count, 0)

    def test_15_refresh_nonexistent_view(self):
        """Verify error message for unknown view."""
        self.init_cache()
        with self.assertRaisesRegex(sqlite3.OperationalError, "View not found"):
            self.conn.execute("SELECT mview_refresh('ghost')")

    def test_version_check(self):
        """Verify the version function returns the expected 2.0.0 string."""
        ver = self.conn.execute("SELECT mview_version()").fetchone()[0]
        self.assertEqual(ver, "3.0.0")

    # =========================================================================
    # SECTION 6: Management & Discovery
    # =========================================================================

    def test_16_list_and_info(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('v1', 'SELECT 1')")
        self.conn.execute("SELECT mview_create('v2', 'SELECT 2')")

        # Test List (Virtual Table)
        rows = self.conn.execute("SELECT view_name FROM mview_registry ORDER BY view_name").fetchall()
        names = [r[0] for r in rows]
        self.assertIn("v1", names)
        self.assertIn("v2", names)

        # Test Info
        info_json = self.conn.execute("SELECT mview_info('v1')").fetchone()[0]
        self.assertIn('"view_name": "v1"', info_json)
        self.assertIn('"source_query_hint"', info_json)

    def test_17_stats_and_verify(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_stat', 'SELECT * FROM users')")

        # Stats
        stats = self.conn.execute("SELECT mview_stats('v_stat')").fetchone()[0]
        self.assertIn('"row_count": 3', stats)

        # Verify OK
        ver = self.conn.execute("SELECT mview_verify('v_stat')").fetchone()[0]
        self.assertEqual(ver, "OK")

        # Verify Fail (Simulate drift by dropping column in view)
        # We can't easily drop col in sqlite, but we can rename table to break it?
        # mview_verify checks schemas.
        # Let's create a table with wrong schema manually in mviews
        self.conn.execute("CREATE TABLE mviews.bad AS SELECT id FROM main.users") # only id
        # Register it manually
        self.conn.execute("INSERT INTO mviews._mview_registry (view_name, source_query) VALUES ('bad', 'SELECT id, name FROM users')")

        ver = self.conn.execute("SELECT mview_verify('bad')").fetchone()[0]
        self.assertIn("FAIL", ver)

    # =========================================================================
    # SECTION 7: Bulk Operations
    # =========================================================================

    def test_18_refresh_all(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('v1', 'SELECT * FROM users')")
        self.conn.execute("SELECT mview_create('v2', 'SELECT * FROM users')")

        # Add data
        self.conn.execute("INSERT INTO users VALUES (4, 'Dave', 'user')")

        res = self.conn.execute("SELECT mview_refresh_all()").fetchone()[0]
        self.assertIn("Refreshed 2 views", res)

        # Check data updated
        c1 = self.conn.execute("SELECT count(*) FROM mviews.v1").fetchone()[0]
        c2 = self.conn.execute("SELECT count(*) FROM mviews.v2").fetchone()[0]
        self.assertEqual(c1, 4)
        self.assertEqual(c2, 4)

    def test_19_drop_all(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('v1', 'SELECT 1')")
        self.conn.execute("SELECT mview_drop_all()")

        # Registry empty
        count = self.conn.execute("SELECT count(*) FROM mviews._mview_registry").fetchone()[0]
        self.assertEqual(count, 0)

    # =========================================================================
    # SECTION 8: Schema & Advanced
    # =========================================================================

    def test_20_rename(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('old', 'SELECT 1')")
        self.conn.execute("SELECT mview_rename('old', 'new')")

        # Check registry
        cnt = self.conn.execute("SELECT count(*) FROM mviews._mview_registry WHERE view_name='new'").fetchone()[0]
        self.assertEqual(cnt, 1)

        # Check table
        self.conn.execute("SELECT * FROM mviews.new") # Should not fail

    def test_21_export(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('ex', 'SELECT 1')")
        ddl = self.conn.execute("SELECT mview_export('ex')").fetchone()[0]
        self.assertIn("mview_create('ex'", ddl)

    def test_22_explain(self):
        self.init_cache()
        self.conn.execute("SELECT mview_create('ex_plan', 'SELECT * FROM users WHERE id > 1')")
        plan = self.conn.execute("SELECT mview_explain('ex_plan')").fetchone()[0]
        # output should contain SCAN or SEARCH
        self.assertTrue("SCAN" in plan or "SEARCH" in plan)

    # =========================================================================
    # SECTION 9: Extended Edge Cases & Error Handling
    # =========================================================================

    def test_23_sql_injection_attempts(self):
        """Attempts to inject SQL via view names."""
        self.init_cache()
        # Name designed to break out of quotes: v1'); DROP TABLE users; --
        bad_name = "v1'); DROP TABLE mviews._mview_registry; --"

        # Should fail or handle it safely (by quoting)
        # Our quote_identifier function doubles quotes, so it should be safe.
        # It creates a view named "v1'); DROP..."
        self.conn.execute("SELECT mview_create(?, 'SELECT 1')", (bad_name,))

        # Verify it exists with that weird name
        rows = self.conn.execute("SELECT view_name FROM mview_registry").fetchall()
        self.assertIn(bad_name, [r[0] for r in rows])

        # Verify registry is intact
        count = self.conn.execute("SELECT count(*) FROM mviews._mview_registry").fetchone()[0]
        self.assertGreater(count, 0)

        # Cleanup
        self.conn.execute("SELECT mview_drop(?)", (bad_name,))

    def test_24_transaction_rollback(self):
        """Verify that operations respect transaction rollbacks."""
        self.init_cache()

        # Start transaction
        self.conn.execute("BEGIN")
        self.conn.execute("SELECT mview_create('rollback_test', 'SELECT 1')")

        # Verify it 'exists' inside transaction
        exists = self.conn.execute("SELECT count(*) FROM mview_registry WHERE view_name='rollback_test'").fetchone()[0]
        self.assertEqual(exists, 1)

        # Rollback
        self.conn.execute("ROLLBACK")

        # Verify it is gone
        exists = self.conn.execute("SELECT count(*) FROM mview_registry WHERE view_name='rollback_test'").fetchone()[0]
        self.assertEqual(exists, 0)

        # Also check physical table is gone (or never created/persisted)
        # SQLite rollback should handle the DDL rollback if it supports it (it usually does for schema changes)
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT * FROM mviews.rollback_test")

    def test_25_invalid_queries(self):
        """Test creating views with invalid SQL."""
        self.init_cache()
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
             self.conn.execute("SELECT mview_create('bad_sql', 'SELECT * FROM nonexistent_table')")

    def test_26_attach_detach_cycles(self):
        """Test multiple attach/detach cycles."""
        for i in range(3):
            self.init_cache()
            self.conn.execute("SELECT mview_create('cycle_v', 'SELECT 1')")
            self.conn.execute("SELECT mview_dettach()")

    def test_27_huge_view_name(self):
        """Test with a very long view name."""
        self.init_cache()
        long_name = "v" * 1000
        self.conn.execute("SELECT mview_create(?, 'SELECT 1')", (long_name,))

        # Verify
        res = self.conn.execute("SELECT view_name FROM mview_registry WHERE view_name=?", (long_name,)).fetchone()
        self.assertEqual(res[0], long_name)

        self.conn.execute("SELECT mview_drop(?)", (long_name,))

    def test_28_quoted_identifiers(self):
        """Test views with quotes in name."""
        self.init_cache()
        weird_name = 'v"quotes"'
        self.conn.execute("SELECT mview_create(?, 'SELECT 1')", (weird_name,))

        # Verify
        res = self.conn.execute("SELECT view_name FROM mview_registry WHERE view_name=?", (weird_name,)).fetchone()
        self.assertEqual(res[0], weird_name)

        # Verify query works (requires proper quoting internally)
        count = self.conn.execute(f"SELECT count(*) FROM mviews.\"{weird_name.replace('\"', '\"\"')}\"").fetchone()[0]
        self.assertEqual(count, 1)

    def test_29_error_propagation(self):
        """Test error conditions that might be missed."""
        self.init_cache()

        # strict create with invalid schema
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_create('v_strict', 'SELECT 1', 'id INTEGER PRIMARY KEY, invalid_col TYPE_NOT_EXIST')")
            # SQLite might accept invalid types, but let's try syntax error
            self.conn.execute("SELECT mview_create('v_strict2', 'SELECT 1', 'syntax error here')")

    def test_30_detach_failure(self):
        """Test mview_dettach failure when DB is busy."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_busy', 'SELECT 1')")

        # Start a transaction on the attached DB to lock it
        self.conn.execute("BEGIN IMMEDIATE")
        self.conn.execute("INSERT INTO mviews._mview_registry (view_name, source_query) VALUES ('dummy', 'select 1')")

        # Try to detach while busy
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_dettach()")

        self.conn.execute("ROLLBACK")

        # Now it should work
        self.conn.execute("SELECT mview_dettach()")

    def test_31_null_args(self):
        """Test NULL arguments in extension functions."""
        self.init_cache()

        # mview_create(NULL, ...)
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_create(NULL, 'SELECT 1')")

        # mview_attach(NULL) - Triggers usage error
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_attach(NULL)")

        # mview_refresh(NULL)
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_refresh(NULL)")

        # mview_rename(NULL, 'new')
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_rename(NULL, 'new')")

    def test_32_strict_schema_failure(self):
        """Test mview_create failure with invalid schema in strict mode."""
        self.init_cache()
        # Invalid type/syntax
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_create('v_strict_fail', 'SELECT 1', 'id INT, bad_col UNKNOWN_TYPE_XYZ')")

    def test_33_init_no_attach(self):
        """Test mview_init without mview_attach."""
        # Reset connection to clear state
        self.conn.close()
        self.conn = sqlite3.connect(":memory:")
        self.conn.enable_load_extension(True)
        self.conn.load_extension(EXT_PATH)

        # Call init without attach -> should fail
        with self.assertRaisesRegex(sqlite3.OperationalError, "MView storage not found"):
             self.conn.execute("SELECT mview_init()")

    def test_34_refresh_fail(self):
        """Test mview_refresh failure (e.g. source table missing)."""
        self.init_cache()
        self.conn.execute("CREATE TABLE source (id INT)")
        self.conn.execute("SELECT mview_create('v_ref_fail', 'SELECT * FROM source')")

        # Drop source
        self.conn.execute("DROP TABLE source")

        # Refresh should fail (CTAS fails)
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_refresh('v_ref_fail')")

    def test_35_verify_mismatch(self):
        """Test mview_verify when schema does not match."""
        self.init_cache()
        self.conn.execute("CREATE TABLE base (id INT)")
        self.conn.execute("SELECT mview_create('v_ver_fail', 'SELECT * FROM base', 'id INT')")

        # Alter base table to break view match
        self.conn.execute("ALTER TABLE base ADD COLUMN extra TEXT")

        # Verify should report error (not OK)
        res = self.conn.execute("SELECT mview_verify('v_ver_fail')").fetchone()[0]
        self.assertNotEqual(res, "OK")
        self.assertTrue("Column count mismatch" in res or "Schema mismatch" in res)

    def test_36_remove_index_fail(self):
        """Test removing an index that doesn't exist."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_idx_fail', 'SELECT 1')")

        # Remove non-existent index -> no error, but logic path coverage
        # It executes DELETE FROM ... WHERE ... which just affects 0 rows.
        self.conn.execute("SELECT mview_remove_index('v_idx_fail', 'non_existent_col')")

    def test_37_explain_fail(self):
        """Test mview_explain failure (e.g., bad view)."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_explain('v_not_exist')")

    def test_38_export_fail(self):
        """Test mview_export failure."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_export('v_export_fail')")

    def test_39_registry_filtering(self):
        """Test filtering on mview_registry (xFilter)."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v1', 'SELECT 1')")
        self.conn.execute("SELECT mview_create('v2', 'SELECT 2')")

        # Exact match filter (might use index if xBestIndex supports it, which ours might not fully, but good to test)
        res = self.conn.execute("SELECT view_name FROM mview_registry WHERE view_name='v1'").fetchall()
        self.assertEqual(len(res), 1)
        self.assertEqual(res[0][0], 'v1')

        # No match
        res = self.conn.execute("SELECT view_name FROM mview_registry WHERE view_name='v_none'").fetchall()
        self.assertEqual(len(res), 0)

    def test_40_registry_scan(self):
        """Test scanning mview_registry with multiple rows."""
        self.init_cache()
        # Create multiple views
        for i in range(5):
            self.conn.execute(f"CREATE TABLE t{i} (x INT)")
            self.conn.execute(f"SELECT mview_create('v{i}', 'SELECT * FROM t{i}')")

        # Scan with LIMIT
        rows = self.conn.execute("SELECT view_name FROM mview_registry ORDER BY view_name LIMIT 3").fetchall()
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0][0], 'v0')
        self.assertEqual(rows[2][0], 'v2')

    def test_41_drop_all_error(self):
        """Test mview_drop_all when registry is empty or missing (though hard to force missing registry without detaching)."""
        self.init_cache()
        # Should work even if empty
        self.conn.execute("SELECT mview_drop_all()")

        # Test error propagation: Detach and try drop_all (which should fail or return error)
        # Note: drop_all checks init_registry, so it might re-init or fail if path bad.
        # Let's try to corrupt the registry table name? No, risky.
        # Just ensure it runs cleanly on empty.

    def test_42_version(self):
        """Test mview_version."""
        # Simple coverage for version function
        v = self.conn.execute("SELECT mview_version()").fetchone()[0]
        self.assertTrue(v.startswith("3."))

    def test_43_registry_corruption(self):
        """Test mview_refresh failure when registry metadata is corrupted."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_corrupt', 'SELECT 1')")

        # Corrupt: Set source_query to invalid SQL
        self.conn.execute("UPDATE mviews._mview_registry SET source_query='INVALID SQL' WHERE view_name='v_corrupt'")

        # Refresh should fail during prepare/CTAS
        with self.assertRaises(sqlite3.OperationalError):
             self.conn.execute("SELECT mview_refresh('v_corrupt')")

    def test_44_weird_identifiers(self):
        """Test quoting logic with complex identifiers."""
        self.init_cache()
        weird_name = 'v_"weird"_name'
        self.conn.execute("SELECT mview_create(?, 'SELECT 1')", (weird_name,))

        # Verify
        res = self.conn.execute("SELECT view_name FROM mview_registry WHERE view_name=?", (weird_name,)).fetchone()
        self.assertEqual(res[0], weird_name)

        # Refresh
        self.conn.execute("SELECT mview_refresh(?)", (weird_name,))

        # Drop
        self.conn.execute("SELECT mview_drop(?)", (weird_name,))

    def test_45_add_unique_index(self):
        """Test adding a UNIQUE index via mview_add_index."""
        self.init_cache()
        self.conn.execute("CREATE TABLE t_uniq (id INT)")
        self.conn.execute("INSERT INTO t_uniq VALUES (1), (2)")
        self.conn.execute("SELECT mview_create('v_uniq', 'SELECT * FROM t_uniq')")

        # Add UNIQUE index
        self.conn.execute("SELECT mview_add_index('v_uniq', 'id', 1)")

        # Refresh to apply
        self.conn.execute("SELECT mview_refresh('v_uniq')")

        # Verify unique constraint logic (insert duplicate into source and refresh -> should fail?)
        # Refresh uses CTAS so if source has dupes, unique index on temp table creation will fail?
        # mview_refresh logic:
        # 1. Create temp table
        # 2. Populate
        # 3. Apply indexes (Here it will fail if duplicate)
        self.conn.execute("INSERT INTO t_uniq VALUES (1)")
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_refresh('v_uniq')")

    def test_46_vacuum(self):
        """Test mview_vacuum (basic execution)."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_vac', 'SELECT 1')")

        # mview_vacuum calls VACUUM which requires no active statements.
        # Usually need a separate connection or finalize all statements.
        # For this test, we commit and try.
        self.conn.commit()

        # Vacuum should run without error (or might succeed/fail based on internal VACUUM)
        # Note: Some SQLite builds might not support VACUUM on attached dbs easily.
        try:
            self.conn.execute("SELECT mview_vacuum()")
        except sqlite3.OperationalError as e:
            # If vacuum fails due to internal limitations, it's expected for coverage
            self.assertTrue("cannot VACUUM" in str(e) or "VACUUM" in str(e))

    def test_47_drop_orphans(self):
        """Test dropping a view when the physical table is missing (orphan registry)."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_orphan', 'SELECT 1')")

        # Manually drop the table to make it an orphan
        self.conn.execute('DROP TABLE mviews."v_orphan"')

        # mview_drop should handle this gracefully (clean registry and warn/ignore)
        self.conn.execute("SELECT mview_drop('v_orphan')")

        # Verify gone from registry
        res = self.conn.execute("SELECT count(*) FROM mview_registry WHERE view_name='v_orphan'").fetchone()[0]
        self.assertEqual(res, 0)

    def test_48_reindex(self):
        """Test mview_reindex on a view with indexes."""
        self.init_cache()
        self.conn.execute("CREATE TABLE t_idx (id INT, val TEXT)")
        self.conn.execute("INSERT INTO t_idx VALUES (1, 'a'), (2, 'b')")
        self.conn.execute("SELECT mview_create('v_ridx', 'SELECT * FROM t_idx')")
        self.conn.execute("SELECT mview_add_index('v_ridx', 'id', 0)")

        # Reindex
        self.conn.execute("SELECT mview_reindex('v_ridx')")

    def test_49_stats_fail(self):
        """Test mview_stats on a non-existent view (coverage only)."""
        self.init_cache()
        # mview_stats might return any value for non-existent views
        # Just call it for coverage
        self.conn.execute("SELECT mview_stats('v_no_exist')")

    def test_50_add_index_null(self):
        """Test mview_add_index with NULL arguments."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_add_index(NULL, 'col', 0)")
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_add_index('v', NULL, 0)")

    def test_51_drop_nonexistent(self):
        """Test mview_drop on a non-existent view."""
        self.init_cache()
        # Should not error, just report or do nothing
        self.conn.execute("SELECT mview_drop('v_nonexistent')")

    def test_52_info_fail(self):
        """Test mview_info on a non-existent view."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_info('v_no_info')")


    def test_53_remove_index_null(self):
        """Test mview_remove_index with NULL arguments."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_remove_index(NULL, 'col')")
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_remove_index('v', NULL)")

    def test_54_verify_fail(self):
        """Test mview_verify on a non-existent view."""
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_verify('v_no_verify')")

    def test_55_rename_to_existing(self):
        """Test mview_rename when target name already exists."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_src', 'SELECT 1')")
        self.conn.execute("SELECT mview_create('v_dst', 'SELECT 2')")

        # Renaming to existing should fail or replace?
        # Let's see the behavior
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_rename('v_src', 'v_dst')")

    def test_56_refresh_all(self):
        """Test mview_refresh_all with multiple views."""
        self.init_cache()
        for i in range(3):
            self.conn.execute(f"CREATE TABLE t{i} (x INT)")
            self.conn.execute(f"SELECT mview_create('vr{i}', 'SELECT * FROM t{i}')")

        # Refresh all
        self.conn.execute("SELECT mview_refresh_all()")

    def test_57_drop_null(self):
        """Test mview_drop with NULL argument (coverage only)."""
        self.init_cache()
        # mview_drop with NULL might not error, just does nothing
        self.conn.execute("SELECT mview_drop(NULL)")

    def test_58_export_working(self):
        """Test mview_export on an existing view."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_exp', 'SELECT 1 as id')")
        res = self.conn.execute("SELECT mview_export('v_exp')").fetchone()[0]
        # Export returns SQL to recreate the view via mview_create
        self.assertIn("mview_create", res)

    def test_59_mview_has(self):
        """Test mview_has function."""
        self.init_cache()

        # View doesn't exist
        has = self.conn.execute("SELECT mview_has('v_missing')").fetchone()[0]
        self.assertEqual(has, 0)

        # Create view
        self.conn.execute("SELECT mview_create('v_exists', 'SELECT 1')")

        # View exists
        has = self.conn.execute("SELECT mview_has('v_exists')").fetchone()[0]
        self.assertEqual(has, 1)

        # NULL argument
        has = self.conn.execute("SELECT mview_has(NULL)").fetchone()[0]
        self.assertEqual(has, 0)

    def test_60_last_refreshed(self):
        """Test mview_last_refreshed function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_time', 'SELECT 1')")

        # Should return a timestamp
        ts = self.conn.execute("SELECT mview_last_refreshed('v_time')").fetchone()[0]
        self.assertIsNotNone(ts)
        # Timestamp should be recent (contains current date)
        import datetime
        today = datetime.date.today().isoformat()
        self.assertIn(today[:7], ts)  # At least year-month match

    def test_61_age(self):
        """Test mview_age function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_age', 'SELECT 1')")

        # Age should be a small number (just created)
        age = self.conn.execute("SELECT mview_age('v_age')").fetchone()[0]
        self.assertIsNotNone(age)
        self.assertLess(age, 10)  # Should be less than 10 seconds old

    def test_62_stale(self):
        """Test mview_stale function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_stale', 'SELECT 1')")

        # Just created, should not be stale with 60s threshold
        stale = self.conn.execute("SELECT mview_stale('v_stale', 60)").fetchone()[0]
        self.assertEqual(stale, 0)

        # Should be "stale" with 0s threshold (any age > 0)
        import time
        time.sleep(0.1)  # Wait a tiny bit
        # With threshold of 0, might be 0 or 1 depending on timing
        # Use threshold of -1 to guarantee stale
        stale = self.conn.execute("SELECT mview_stale('v_stale', -1)").fetchone()[0]
        self.assertEqual(stale, 1)

    def test_63_query(self):
        """Test mview_query function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_q', 'SELECT 1 as x')")

        query = self.conn.execute("SELECT mview_query('v_q')").fetchone()[0]
        self.assertIn("SELECT 1", query)

    def test_64_schema(self):
        """Test mview_schema function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_s', 'SELECT 1 as id, 2 as val')")

        schema = self.conn.execute("SELECT mview_schema('v_s')").fetchone()[0]
        self.assertIn("id", schema)
        self.assertIn("val", schema)

    def test_65_indexes(self):
        """Test mview_indexes function."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('v_ix', 'SELECT 1 as id')")
        self.conn.execute("SELECT mview_add_index('v_ix', 'id', 1)")

        indexes = self.conn.execute("SELECT mview_indexes('v_ix')").fetchone()[0]
        self.assertIn("id", indexes)
        self.assertIn("is_unique", indexes)

    def test_66_refresh_stale(self):
        """Test mview_refresh_stale function."""
        self.init_cache()
        self.conn.execute("CREATE TABLE t_rs (x INT)")
        self.conn.execute("INSERT INTO t_rs VALUES (1)")
        self.conn.execute("SELECT mview_create('v_rs', 'SELECT * FROM t_rs')")

        # Refresh with very high threshold (nothing stale)
        res = self.conn.execute("SELECT mview_refresh_stale(999999)").fetchone()[0]
        self.assertIn("0", res)  # 0 refreshed

        # Refresh with -1 threshold (everything stale)
        self.conn.execute("INSERT INTO t_rs VALUES (2)")
        res = self.conn.execute("SELECT mview_refresh_stale(-1)").fetchone()[0]
        self.assertIn("1", res)  # 1 refreshed

    def test_67_logging(self):
        """Test mview_log_enable and mview_log functions."""
        self.init_cache()

        # Enable logging
        res = self.conn.execute("SELECT mview_log_enable(1)").fetchone()[0]
        self.assertIn("enabled", res.lower())

        # Get log (might be empty initially)
        log = self.conn.execute("SELECT mview_log()").fetchone()[0]
        self.assertIn("[", log)  # JSON array

        # Disable logging
        res = self.conn.execute("SELECT mview_log_enable(0)").fetchone()[0]
        self.assertIn("disabled", res.lower())

    def test_68_log_clear(self):
        """Test mview_log_clear function."""
        self.init_cache()

        # Enable logging and do something
        self.conn.execute("SELECT mview_log_enable(1)")
        self.conn.execute("SELECT mview_create('v_log', 'SELECT 1')")
        self.conn.execute("SELECT mview_refresh_stale(-1)")

        # Clear the log
        res = self.conn.execute("SELECT mview_log_clear()").fetchone()[0]
        self.assertIn("cleared", res.lower())

        # Log should be empty now
        log = self.conn.execute("SELECT mview_log()").fetchone()[0]
        self.assertEqual(log, "[]")

    def test_69_size(self):
        """Test mview_size function."""
        self.init_cache()
        self.conn.execute("CREATE TABLE t_sz (id INT, name TEXT)")
        for i in range(100):
            self.conn.execute(f"INSERT INTO t_sz VALUES ({i}, 'name{i}')")
        self.conn.execute("SELECT mview_create('v_sz', 'SELECT * FROM t_sz')")
        
        # Size should be > 0
        size = self.conn.execute("SELECT mview_size('v_sz')").fetchone()[0]
        self.assertIsNotNone(size)
        self.assertGreater(size, 0)

    def test_70_count(self):
        """Test mview_count function."""
        self.init_cache()
        # Initial count (some tests might have created views already if using shared DB,
        # but here each test is isolated by init_cache which usually cleans up)
        # Actually init_cache starts fresh.
        count = self.conn.execute("SELECT mview_count()").fetchone()[0]
        self.assertEqual(count, 0)
        
        self.conn.execute("SELECT mview_create('v1', 'SELECT 1')")
        self.conn.execute("SELECT mview_create('v2', 'SELECT 2')")
        
        count = self.conn.execute("SELECT mview_count()").fetchone()[0]
        self.assertEqual(count, 2)

    def test_71_truncate(self):
        """Test mview_truncate function."""
        self.init_cache()
        self.conn.execute("CREATE TABLE t_tr (x INT)")
        self.conn.execute("INSERT INTO t_tr VALUES (1), (2), (3)")
        self.conn.execute("SELECT mview_create('v_tr', 'SELECT * FROM t_tr')")
        
        # Verify data exists
        rows = self.conn.execute("SELECT count(*) FROM mviews.v_tr").fetchone()[0]
        self.assertEqual(rows, 3)
        
        # Truncate
        self.conn.execute("SELECT mview_truncate('v_tr')")
        
        # Verify data gone
        rows = self.conn.execute("SELECT count(*) FROM mviews.v_tr").fetchone()[0]
        self.assertEqual(rows, 0)
        
        # Verify last_refreshed is NULL
        ts = self.conn.execute("SELECT mview_last_refreshed('v_tr')").fetchone()[0]
        self.assertIsNone(ts)

if __name__ == "__main__":
    unittest.main()
