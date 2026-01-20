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
DB_PATH = "test_cache.db"

class TestMViewExtension(unittest.TestCase):
    """
    Test Suite for sqlite-mview Extension.

    Architecture Overview for Testers:
    ----------------------------------
    1.  **Attached DB**: The extension does not store data in the main DB.
        It attaches a separate file (DB_PATH) as the schema 'mviews'.
    2.  **Registry**: Two tables in 'mviews' track state:
        - `_mview_registry`: Stores view names and source SQL.
        - `_mview_index_registry`: Stores index definitions.
    3.  **Shadow Paging (Refresh)**:
        - A refresh does NOT delete/insert into the live table.
        - It creates `view_new_RANDOM`, populates it, indexes it,
          then swaps names in a transaction.
    """

    def setUp(self):
        """
        Per-test setup.
        1. Connects to in-memory DB.
        2. Sets isolation_level=None (Auto-commit). This is CRITICAL.
           If Python manages transactions implicitly, it conflicts with the
           extension's 'BEGIN IMMEDIATE' commands during refreshes.
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
            self.conn.execute("SELECT mview_close()")
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
        self.conn.execute(f"SELECT mview_init('{DB_PATH}')")

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
        with self.assertRaisesRegex(sqlite3.OperationalError, "Cache DB not found"):
            self.conn.execute("SELECT mview_create('fail', 'SELECT 1')")

    def test_03_double_init_is_idempotent(self):
        """
        Verify that calling init twice is safe and detects the existing attachment.
        It should return a specific success message, not an error.
        """
        self.init_cache()

        # Second call
        cursor = self.conn.execute(f"SELECT mview_init('{DB_PATH}')")
        result = cursor.fetchone()[0]

        self.assertIn("Already Attached", result)

    def test_04_init_invalid_path(self):
        """Verify behavior when checking an invalid path (e.g. directory)."""
        # '.' is a directory, sqlite3_open will fail or return error
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_init('.')")

    def test_05_close_detaches_db(self):
        """Verify mview_close removes the schema from the connection."""
        self.init_cache()
        self.conn.execute("SELECT mview_close()")

        # Check SQLite's internal list of attached databases
        schemas = [row[1] for row in self.conn.execute("PRAGMA database_list")]
        self.assertNotIn("mviews", schemas)

    # =========================================================================
    # SECTION 2: View Creation & Schema
    # =========================================================================

    def test_06_simple_create(self):
        """Verify standard 'Create Table As Select' behavior."""
        self.init_cache()
        sql = "SELECT role, count(*) as cnt FROM main.users GROUP BY role"
        self.conn.execute("SELECT mview_create(?, ?)", ("stats", sql))

        # Query the materialized table
        rows = self.conn.execute("SELECT * FROM mviews.stats ORDER BY role").fetchall()
        expected = [("admin", 1), ("user", 2)]
        self.assertEqual(rows, expected)

    def test_07_strict_create_enforces_pk(self):
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

    def test_08_strict_create_column_mismatch(self):
        """
        Verify error when strict schema doesn't match query columns.
        Schema has 3 columns, Query returns 2.
        """
        self.init_cache()
        schema = "id INT, name TEXT, extra INT" # 3 cols
        query = "SELECT id, name FROM main.users" # 2 cols

        with self.assertRaisesRegex(sqlite3.OperationalError, "values were supplied"):
            self.conn.execute("SELECT mview_create('mismatch', ?, ?)", (query, schema))

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

    def test_10_sql_injection_safety(self):
        """
        Verify that table names with special characters are quoted correctly
        internally and do not break the SQL parser.
        """
        self.init_cache()
        weird_name = 'my " view -- table'

        # This shouldn't crash or drop tables
        self.conn.execute("SELECT mview_create(?, 'SELECT 1')", (weird_name,))

        # To query a table with a double quote in its name, standard SQL requires
        # escaping the quote by doubling it.
        # Python String: 'my " view -- table'
        # SQL Identifier: "my "" view -- table"
        escaped_name = weird_name.replace('"', '""')

        count = self.conn.execute(f'SELECT count(*) FROM mviews."{escaped_name}"').fetchone()[0]
        self.assertEqual(count, 1)

    # =========================================================================
    # SECTION 3: Refresh Mechanics & Shadow Paging
    # =========================================================================

    def test_11_refresh_updates_data(self):
        """Verify refresh captures changes from source."""
        self.init_cache()
        self.conn.execute("SELECT mview_create('users_list', 'SELECT name FROM main.users')")

        # Modify Source
        self.conn.execute("INSERT INTO users VALUES (4, 'David', 'admin')")

        # Refresh
        self.conn.execute("SELECT mview_refresh('users_list')")

        count = self.conn.execute("SELECT count(*) FROM mviews.users_list").fetchone()[0]
        self.assertEqual(count, 4)

    def test_12_refresh_rollback_on_source_error(self):
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

    def test_13_index_persistence(self):
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

    def test_14_unique_constraint_enforcement(self):
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

    def test_15_refresh_fails_on_broken_index_schema(self):
        """
        Verify handling of Schema Drift.
        Scenario:
          1. View matches Source.
          2. Index exists on Col 'A'.
          3. User changes Source SQL via overwrite (or source table changes) such that Col 'A' is gone.
          4. Refresh tries to create Index on 'A', but 'A' is missing in new table.
          5. Must fail and rollback.
        """
        self.init_cache()
        # 1. Create View with 'name' and 'role'
        self.conn.execute("SELECT mview_create('drift', 'SELECT name, role FROM users')")

        # 2. Add Index on 'role'
        self.conn.execute("SELECT mview_add_index('drift', 'role', 0)")

        # 3. "Drift": Update registry to remove 'role' from source query
        # We manually corrupt the registry for this test case to simulate a bad change
        self.conn.execute("""
            UPDATE mviews._mview_registry
            SET source_query = 'SELECT name FROM users'
            WHERE view_name = 'drift'
        """)

        # 4. Refresh
        # The C code will: Create temp table (has only 'name'). Try to create index on 'role'. Fail.
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such column: role"):
            self.conn.execute("SELECT mview_refresh('drift')")

        # 5. Verify Rollback (Old table with 'role' still exists)
        cols = [row[1] for row in self.conn.execute("PRAGMA mviews.table_info(drift)").fetchall()]
        self.assertIn("role", cols)

    # =========================================================================
    # SECTION 5: Cleanup & Edge Cases
    # =========================================================================

    def test_16_drop_cleanup(self):
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

    def test_17_refresh_nonexistent_view(self):
        """Verify error message for unknown view."""
        self.init_cache()
        with self.assertRaisesRegex(sqlite3.OperationalError, "View not found"):
            self.conn.execute("SELECT mview_refresh('ghost')")

    def test_version_check(self):
        """Verify the version function returns the expected 2.0.0 string."""
        ver = self.conn.execute("SELECT mview_version()").fetchone()[0]
        self.assertEqual(ver, "2.0.0")

if __name__ == "__main__":
    unittest.main()
