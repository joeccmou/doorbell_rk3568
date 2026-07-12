"""验证 event_store.cpp 中的 SQLite 迁移 SQL，不修改输入数据库。"""

import pathlib
import re
import shutil
import sqlite3
import sys
import tempfile


source_db = pathlib.Path(sys.argv[1])
source = pathlib.Path("src/events/event_store.cpp").read_text(encoding="utf-8")
match = re.search(
    r'static const char \*migration = R"SQL\((.*?)\)SQL";', source, re.S
)
if match is None:
    raise RuntimeError("找不到 SQLite migration SQL")

with tempfile.TemporaryDirectory() as temp_dir:
    test_db = pathlib.Path(temp_dir) / "doorbell.db"
    shutil.copy2(source_db, test_db)
    connection = sqlite3.connect(test_db)
    try:
        connection.executescript(match.group(1))
        version = connection.execute("PRAGMA user_version").fetchone()[0]
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(events)")
        }
        row_counts = connection.execute(
            """
            SELECT COUNT(*),
                   SUM(occurred_timezone='Asia/Shanghai'),
                   SUM(occurred_utc_offset_minutes=480),
                   SUM(occurred_local_date=strftime(
                       '%Y-%m-%d', at_ms / 1000.0, 'unixepoch', '+480 minutes'))
            FROM events
            """
        ).fetchone()
        tables = {
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }
        required = {
            "occurred_timezone",
            "occurred_utc_offset_minutes",
            "occurred_local_date",
        }
        assert version == 2
        assert required <= columns
        assert "clip_daily_sequences" in tables
        assert row_counts[0] == row_counts[1] == row_counts[2] == row_counts[3]
        print(
            f"SQLite migration verified: version={version}, "
            f"events={row_counts[0]}, historical_rows_preserved={row_counts[1]}"
        )
    finally:
        connection.close()
