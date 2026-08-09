"""
SQLite-backed asset index — see phi.md's "Asset tracking and the Asset
Browser panel" section for the design this implements.

This is an index, not a source of truth: the actual asset bytes live as
.glb files under www/assets/library/; this just tracks path/name/tags so
they're searchable. The DB file lives outside www/ (server/assets.db) so
it's never accidentally served as a static file.

Single shared connection + a lock, matching server.py's existing
GameWorld-style "one lock per shared resource" pattern -- asset CRUD
volume is low (editor-driven, not a hot per-frame path), so simple
serialization is the right tradeoff over anything fancier.
"""

import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS asset (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    path       TEXT NOT NULL,
    name       TEXT NOT NULL,
    created_at REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS tag (
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    tag      TEXT NOT NULL
);
"""


class AssetDB:
    def __init__(self, db_path: str):
        self._conn = sqlite3.connect(db_path, check_same_thread=False)
        self._conn.execute("PRAGMA foreign_keys = ON")
        self._conn.executescript(SCHEMA)
        self._conn.commit()
        self._lock = threading.Lock()

    def _row_to_dict(self, row) -> dict:
        aid, path, name, created_at = row
        tags = [t for (t,) in self._conn.execute(
            "SELECT tag FROM tag WHERE asset_id = ? ORDER BY tag", (aid,))]
        return {"id": aid, "path": path, "name": name,
                "created_at": created_at, "tags": tags}

    def list_assets(self, query: str = None) -> list[dict]:
        with self._lock:
            if query:
                like = f"%{query}%"
                rows = self._conn.execute(
                    "SELECT DISTINCT a.id, a.path, a.name, a.created_at FROM asset a "
                    "LEFT JOIN tag t ON t.asset_id = a.id "
                    "WHERE a.name LIKE ? OR t.tag LIKE ? ORDER BY a.id",
                    (like, like)).fetchall()
            else:
                rows = self._conn.execute(
                    "SELECT id, path, name, created_at FROM asset ORDER BY id").fetchall()
            return [self._row_to_dict(r) for r in rows]

    def get_asset(self, asset_id: int) -> dict | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT id, path, name, created_at FROM asset WHERE id = ?",
                (asset_id,)).fetchone()
            return self._row_to_dict(row) if row else None

    def create_asset(self, name: str, path: str, tags: list[str]) -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO asset (path, name, created_at) VALUES (?, ?, ?)",
                (path, name, time.time()))
            asset_id = cur.lastrowid
            for tag in tags:
                tag = tag.strip()
                if tag:
                    self._conn.execute(
                        "INSERT INTO tag (asset_id, tag) VALUES (?, ?)", (asset_id, tag))
            self._conn.commit()
            return asset_id

    def update_asset(self, asset_id: int, name: str, tags: list[str]) -> bool:
        with self._lock:
            cur = self._conn.execute(
                "UPDATE asset SET name = ? WHERE id = ?", (name, asset_id))
            if cur.rowcount == 0:
                self._conn.rollback()
                return False
            self._conn.execute("DELETE FROM tag WHERE asset_id = ?", (asset_id,))
            for tag in tags:
                tag = tag.strip()
                if tag:
                    self._conn.execute(
                        "INSERT INTO tag (asset_id, tag) VALUES (?, ?)", (asset_id, tag))
            self._conn.commit()
            return True

    def delete_asset(self, asset_id: int) -> str | None:
        """Deletes the DB row (and its tags, via ON DELETE CASCADE) and
        returns the asset's path so the caller can unlink the backing
        file -- deleting the file is the caller's job, not this module's,
        since this module only ever touches the index."""
        with self._lock:
            row = self._conn.execute(
                "SELECT path FROM asset WHERE id = ?", (asset_id,)).fetchone()
            if not row:
                return None
            self._conn.execute("DELETE FROM asset WHERE id = ?", (asset_id,))
            self._conn.commit()
            return row[0]
