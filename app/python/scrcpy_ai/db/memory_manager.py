"""MemoryManager: SQLite (action metadata) + ChromaDB (CLIP embeddings)."""

import hashlib
import logging
import os
import sqlite3
import time
from typing import Optional

import chromadb
import numpy as np

from scrcpy_ai.config import config

logger = logging.getLogger(__name__)

_CREATE_TABLE = """
CREATE TABLE IF NOT EXISTS experiences (
    state_id        TEXT PRIMARY KEY,
    image_path      TEXT NOT NULL,
    phash           TEXT,
    created_at      REAL NOT NULL,
    updated_at      REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS actions (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    state_id        TEXT NOT NULL,
    action_type     TEXT NOT NULL DEFAULT 'click',
    x               INTEGER NOT NULL,
    y               INTEGER NOT NULL,
    x2              INTEGER,
    y2              INTEGER,
    extra_json      TEXT,
    execution_count INTEGER NOT NULL DEFAULT 1,
    success_count   INTEGER NOT NULL DEFAULT 0,
    last_executed   REAL NOT NULL,
    created_at      REAL NOT NULL,
    FOREIGN KEY (state_id) REFERENCES experiences(state_id)
);

CREATE INDEX IF NOT EXISTS idx_actions_state ON actions(state_id);
CREATE INDEX IF NOT EXISTS idx_actions_coords ON actions(state_id, action_type, x, y);
"""


class MemoryManager:
    def __init__(self):
        self._db: Optional[sqlite3.Connection] = None
        self._chroma: Optional[chromadb.ClientAPI] = None
        self._collection = None
        self._initialized = False

    def _ensure_init(self):
        if self._initialized:
            return
        os.makedirs(config.db_dir, exist_ok=True)

        # SQLite
        db_path = os.path.join(config.db_dir, "memory.db")
        self._db = sqlite3.connect(db_path, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.executescript(_CREATE_TABLE)
        self._db.commit()

        # ChromaDB
        chroma_path = os.path.join(config.db_dir, "chroma")
        self._chroma = chromadb.PersistentClient(path=chroma_path)
        self._collection = self._chroma.get_or_create_collection(
            name="screen_states",
            metadata={"hnsw:space": "cosine"},
        )

        self._initialized = True
        logger.info("MemoryManager initialized: %s", config.db_dir)

    @staticmethod
    def _make_state_id(embedding: np.ndarray) -> str:
        """Generate a stable state_id from embedding bytes."""
        return hashlib.sha256(embedding.tobytes()).hexdigest()[:16]

    def save_experience(
        self,
        jpeg_bytes: bytes,
        embedding: np.ndarray,
        action_type: str,
        x: int, y: int,
        x2: int | None = None, y2: int | None = None,
        extra_json: str | None = None,
        phash_hex: str | None = None,
        success: bool = True,
    ) -> str:
        """Save a screen state + action to both DBs. Returns state_id."""
        self._ensure_init()
        now = time.time()
        state_id = self._make_state_id(embedding)

        # Save image
        img_dir = os.path.join(config.db_dir, "images")
        os.makedirs(img_dir, exist_ok=True)
        img_path = os.path.join(img_dir, f"{state_id}.jpg")
        if not os.path.exists(img_path):
            with open(img_path, "wb") as f:
                f.write(jpeg_bytes)

        # Upsert experience (screen state)
        self._db.execute(
            """INSERT INTO experiences (state_id, image_path, phash, created_at, updated_at)
               VALUES (?, ?, ?, ?, ?)
               ON CONFLICT(state_id) DO UPDATE SET updated_at=?, phash=COALESCE(?, phash)""",
            (state_id, img_path, phash_hex, now, now, now, phash_hex),
        )

        # Upsert action (find existing by coords proximity)
        coord_tol = 30
        existing = self._db.execute(
            """SELECT id, execution_count, success_count FROM actions
               WHERE state_id=? AND action_type=?
                 AND ABS(x - ?) <= ? AND ABS(y - ?) <= ?
               LIMIT 1""",
            (state_id, action_type, x, coord_tol, y, coord_tol),
        ).fetchone()

        if existing:
            new_exec = existing["execution_count"] + 1
            new_succ = existing["success_count"] + (1 if success else 0)
            self._db.execute(
                """UPDATE actions SET execution_count=?, success_count=?,
                   last_executed=?, x=?, y=?, x2=?, y2=?, extra_json=?
                   WHERE id=?""",
                (new_exec, new_succ, now, x, y, x2, y2, extra_json,
                 existing["id"]),
            )
        else:
            self._db.execute(
                """INSERT INTO actions
                   (state_id, action_type, x, y, x2, y2, extra_json,
                    execution_count, success_count, last_executed, created_at)
                   VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?)""",
                (state_id, action_type, x, y, x2, y2, extra_json,
                 1 if success else 0, now, now),
            )

        self._db.commit()

        # ChromaDB upsert
        self._collection.upsert(
            ids=[state_id],
            embeddings=[embedding.tolist()],
            metadatas=[{"phash": phash_hex or ""}],
        )

        logger.info("Saved experience: state=%s action=%s (%d,%d)",
                     state_id, action_type, x, y)
        return state_id

    def query_experience(
        self,
        current_embedding: np.ndarray,
        top_k: int = 5,
    ) -> list[dict]:
        """Find top_k most similar past experiences with their actions.

        Returns list of dicts:
            {state_id, similarity, actions: [{action_type, x, y, x2, y2,
             execution_count, success_count, extra_json}]}
        """
        self._ensure_init()

        results = self._collection.query(
            query_embeddings=[current_embedding.tolist()],
            n_results=top_k,
        )

        if not results["ids"] or not results["ids"][0]:
            return []

        experiences = []
        for i, state_id in enumerate(results["ids"][0]):
            # ChromaDB returns distances; for cosine space: similarity = 1 - distance
            similarity = 1.0 - results["distances"][0][i]

            # Get actions from SQLite
            rows = self._db.execute(
                """SELECT action_type, x, y, x2, y2, extra_json,
                          execution_count, success_count
                   FROM actions WHERE state_id=?
                   ORDER BY success_count DESC, execution_count ASC""",
                (state_id,),
            ).fetchall()

            actions = []
            for r in rows:
                actions.append({
                    "action_type": r["action_type"],
                    "x": r["x"],
                    "y": r["y"],
                    "x2": r["x2"],
                    "y2": r["y2"],
                    "extra_json": r["extra_json"],
                    "execution_count": r["execution_count"],
                    "success_count": r["success_count"],
                })

            experiences.append({
                "state_id": state_id,
                "similarity": similarity,
                "actions": actions,
            })

        return experiences

    def get_action_count(self, state_id: str, action_type: str,
                         x: int, y: int, coord_tol: int = 30) -> int:
        """Get execution count for a specific action on a state."""
        self._ensure_init()
        row = self._db.execute(
            """SELECT execution_count FROM actions
               WHERE state_id=? AND action_type=?
                 AND ABS(x - ?) <= ? AND ABS(y - ?) <= ?
               LIMIT 1""",
            (state_id, action_type, x, coord_tol, y, coord_tol),
        ).fetchone()
        return row["execution_count"] if row else 0

    def stats(self) -> dict:
        """Return DB statistics."""
        self._ensure_init()
        states = self._db.execute("SELECT COUNT(*) FROM experiences").fetchone()[0]
        actions = self._db.execute("SELECT COUNT(*) FROM actions").fetchone()[0]
        return {"states": states, "actions": actions}


# Global singleton
memory = MemoryManager()
