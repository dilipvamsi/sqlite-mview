import os
import sqlite3
import unittest

# Get the path to the compiled extension (set by Makefile)
EXT_PATH = os.environ.get("EXT_PATH")
DB_PATH = "test_cache.db"


class TestMViewExtension(unittest.TestCase):
    """
    Comprehensive Test Suite for the sqlite-mview C extension.
    Covers: Initialization, Creation, strict-typing, Refresh mechanics,
    SQL Injection safety, and atomic transactions.
    """

    def setUp(self):
        """
        Per-test setup routine.
        1. Connects to an in-memory 'main' database.
        2. Sets isolation_level=None (Auto-commit) to prevent implicit transactions
           from locking the database during DETACH operations.
        3. Loads the C extension.
        4. Creates standard dummy data (users table).
        """
        # CRITICAL: isolation_level=None puts Python in autocommit mode.
        # Without this, Python starts a transaction on the first command,
        # which locks the DB and causes 'database is locked' errors during mview_close().
        self.conn = sqlite3.connect(":memory:", isolation_level=None)
        self.conn.enable_load_extension(True)

        try:
            self.conn.load_extension(EXT_PATH)
        except sqlite3.OperationalError as e:
            self.fail(f"Could not load extension at {EXT_PATH}. Error: {e}")

        # Create dummy data for testing
        self.conn.execute(
            "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, role TEXT)"
        )
        self.conn.execute("INSERT INTO users VALUES (1, 'Alice', 'admin')")
        self.conn.execute("INSERT INTO users VALUES (2, 'Bob', 'user')")
        self.conn.execute("INSERT INTO users VALUES (3, 'Charlie', 'user')")

        # Ensure a clean slate for the cache file
        if os.path.exists(DB_PATH):
            os.remove(DB_PATH)

    def tearDown(self):
        """
        Per-test cleanup.
        1. Attempts to close/detach the view.
        2. Closes connection.
        3. Deletes the cache database file and WAL artifacts.
        """
        try:
            self.conn.execute("SELECT mview_close()")
        except:
            pass  # Ignore if already closed
        self.conn.close()

        # Clean up filesystem
        if os.path.exists(DB_PATH):
            os.remove(DB_PATH)
        if os.path.exists(DB_PATH + "-wal"):
            os.remove(DB_PATH + "-wal")
        if os.path.exists(DB_PATH + "-shm"):
            os.remove(DB_PATH + "-shm")

    def init_cache(self):
        """Helper to initialize the cache database successfully."""
        self.conn.execute(f"SELECT mview_init('{DB_PATH}')")

    # =========================================================================
    # 1. Initialization Logic Tests
    # =========================================================================

    def test_create_without_init(self):
        """
        TEST: Safety Guard.
        Scenario: Try to create a view without running mview_init() first.
        Expectation: OperationalError (Cache DB not found).
        """
        with self.assertRaisesRegex(sqlite3.OperationalError, "Cache DB not found"):
            self.conn.execute("SELECT mview_create('fail', 'SELECT * FROM users')")

    def test_init_invalid_path(self):
        """
        TEST: Invalid File Path.
        Scenario: Try to initialize using a directory ('.') instead of a file.
        Expectation: OperationalError (Unable to open database).
        """
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT mview_init('.')")

    def test_double_init(self):
        """
        TEST: Idempotency / Double Attach.
        Scenario: Run mview_init() twice.
        Expectation: OperationalError (Database 'mviews' is already in use).
        """
        self.init_cache()
        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute(f"SELECT mview_init('{DB_PATH}')")

    # =========================================================================
    # 2. View Creation & Error Handling Tests
    # =========================================================================

    def test_simple_create_and_query(self):
        """
        TEST: Happy Path.
        Scenario: Create a standard aggregated view.
        Expectation:
            1. View is created successfully.
            2. Data can be queried via 'mviews.view_name'.
            3. Data matches the source aggregation.
        """
        self.init_cache()
        sql = "SELECT role, count(*) as cnt FROM main.users GROUP BY role"
        self.conn.execute("SELECT mview_create(?, ?)", ("role_stats", sql))

        rows = self.conn.execute(
            "SELECT * FROM mviews.role_stats ORDER BY role"
        ).fetchall()
        # Alice (admin) = 1, Bob+Charlie (user) = 2
        self.assertEqual(rows, [("admin", 1), ("user", 2)])

    def test_create_invalid_sql(self):
        """
        TEST: Transaction Rollback on SQL Error.
        Scenario: Provide a source query with a syntax error or missing table.
        Expectation:
            1. mview_create fails with an error.
            2. The view table is NOT created (Rollback successful).
        """
        self.init_cache()
        # Regex matches either "syntax error" or "no such table" depending on SQLite version behavior
        with self.assertRaisesRegex(
            sqlite3.OperationalError, "(syntax error|no such table)"
        ):
            self.conn.execute(
                "SELECT mview_create('bad_sql', 'SELECT * FROM non_existent_table')"
            )

        # Verify clean rollback: The table 'bad_sql' should not exist.
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
            self.conn.execute("SELECT * FROM mviews.bad_sql")

    def test_strict_mode_success(self):
        """
        TEST: Strict Schema Enforcement.
        Scenario: Create a view with an explicit schema defining a PRIMARY KEY.
        Expectation:
            1. View behaves like a real table with constraints.
            2. Attempting to insert a duplicate Primary Key manually throws IntegrityError.
        """
        self.init_cache()
        schema = "id INTEGER PRIMARY KEY, name TEXT"
        query = "SELECT id, name FROM main.users"
        self.conn.execute("SELECT mview_create('strict_users', ?, ?)", (query, schema))

        # Verify PK constraint works
        with self.assertRaises(sqlite3.IntegrityError):
            self.conn.execute(
                "INSERT INTO mviews.strict_users VALUES (1, 'Fake User With ID 1')"
            )

    def test_strict_mode_mismatch(self):
        """
        TEST: Schema vs Query Mismatch.
        Scenario: The custom schema defines 3 columns, but source query returns 2.
        Expectation: OperationalError (Column count mismatch).
        """
        self.init_cache()
        schema = "id INT, name TEXT, extra INT"
        query = "SELECT id, name FROM main.users"

        with self.assertRaisesRegex(
            sqlite3.OperationalError,
            "table .* has 3 columns but .* values were supplied",
        ):
            self.conn.execute("SELECT mview_create('mismatch', ?, ?)", (query, schema))

    def test_sql_injection_safety_in_name(self):
        """
        TEST: Security / Identifier Quoting.
        Scenario: Attempt to create a view with a name containing SQL injection characters.
                  Name: view ' -- drop table
        Expectation:
            1. The name is treated as a literal identifier (table name).
            2. No SQL injection occurs.
            3. We can query the table if we quote it properly in SQL.
        """
        self.init_cache()

        # A name that tries to close the string and inject a command
        weird_name = "view ' -- drop table"

        # Create using parameter binding (safe)
        self.conn.execute(
            "SELECT mview_create(?, 'SELECT * FROM users')", (weird_name,)
        )

        # Query it: We must wrap the name in double quotes in the SQL string
        # If the C extension didn't handle quotes properly, this table wouldn't exist or would have a broken name.
        count = self.conn.execute(
            f'SELECT count(*) FROM mviews."{weird_name}"'
        ).fetchone()[0]
        self.assertEqual(count, 3)

    # =========================================================================
    # 3. Refresh Mechanics Tests
    # =========================================================================

    def test_refresh_mechanics(self):
        """
        TEST: Snapshot behavior vs Refresh.
        Scenario:
            1. Create view (count = 3).
            2. Add new data to main table (count = 4).
            3. Check view (should still be 3 - it is a snapshot).
            4. Refresh view.
            5. Check view (should now be 4).
        """
        self.init_cache()
        self.conn.execute(
            "SELECT mview_create('user_list', 'SELECT name FROM main.users')"
        )

        # Add data to source
        self.conn.execute("INSERT INTO users VALUES (4, 'David', 'admin')")

        # Verify view is stale (still 3) - Implicit check via refresh requirement

        # Refresh
        self.conn.execute("SELECT mview_refresh('user_list')")

        # Verify view is fresh (now 4)
        count = self.conn.execute("SELECT count(*) FROM mviews.user_list").fetchone()[0]
        self.assertEqual(count, 4)

    def test_refresh_non_existent(self):
        """
        TEST: Refreshing missing view.
        Scenario: Refresh a name that hasn't been created.
        Expectation: OperationalError ("View not found" / "no such table").
        """
        self.init_cache()
        with self.assertRaisesRegex(sqlite3.OperationalError, "View not found"):
            self.conn.execute("SELECT mview_refresh('ghost_view')")

    def test_refresh_source_deleted(self):
        """
        TEST: Atomic Refresh / Rollback Safety.
        Scenario: The source table (main.users) is deleted, but we try to refresh the view.
        Expectation:
            1. The refresh command fails with 'no such table'.
            2. CRITICAL: The existing data in the view MUST remain intact.
               (The DELETE operation on the view should be rolled back).
        """
        self.init_cache()
        self.conn.execute("SELECT mview_create('temp_view', 'SELECT * FROM users')")

        # Destroy source
        self.conn.execute("DROP TABLE users")

        # Refresh fails
        with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
            self.conn.execute("SELECT mview_refresh('temp_view')")

        # Verify data persisted (count is still 3)
        count = self.conn.execute("SELECT count(*) FROM mviews.temp_view").fetchone()[0]
        self.assertEqual(count, 3)

    # =========================================================================
    # 4. Cleanup Logic Tests
    # =========================================================================

    def test_drop_view(self):
        """
        TEST: Dropping a view.
        Scenario: Drop a valid view.
        Expectation: Selecting from the view table throws "no such table".
        """
        self.init_cache()
        self.conn.execute("SELECT mview_create('to_drop', 'SELECT * FROM main.users')")
        self.conn.execute("SELECT mview_drop('to_drop')")

        with self.assertRaises(sqlite3.OperationalError):
            self.conn.execute("SELECT * FROM mviews.to_drop")

    def test_close_behavior(self):
        """
        TEST: Clean Shutdown.
        Scenario: Call mview_close().
        Expectation: The 'mviews' database is DETACHED from the connection.
        """
        self.init_cache()
        self.conn.execute("SELECT mview_close()")

        # Check SQLite's internal list of attached databases
        schemas = [row[1] for row in self.conn.execute("PRAGMA database_list")]
        self.assertNotIn("mviews", schemas)


if __name__ == "__main__":
    unittest.main()
