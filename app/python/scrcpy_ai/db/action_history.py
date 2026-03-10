"""Sliding window of recent (state_id, action) pairs for loop prevention."""

from collections import deque
from dataclasses import dataclass

from scrcpy_ai.config import config


@dataclass
class HistoryEntry:
    state_id: str
    action_type: str
    x: int
    y: int


class ActionHistoryWindow:
    def __init__(self):
        self._history: deque[HistoryEntry] = deque(
            maxlen=config.history_window_size,
        )

    def add(self, state_id: str, action_type: str, x: int, y: int):
        self._history.append(HistoryEntry(state_id, action_type, x, y))

    def count_action(self, state_id: str, action_type: str, x: int, y: int,
                     coord_tolerance: int = 30) -> int:
        """Count how many times a similar action was performed on this state recently."""
        count = 0
        for e in self._history:
            if e.state_id != state_id:
                continue
            if e.action_type != action_type:
                continue
            if abs(e.x - x) <= coord_tolerance and abs(e.y - y) <= coord_tolerance:
                count += 1
        return count

    def all_tried(self, state_id: str, candidates: list[dict],
                  coord_tolerance: int = 30) -> bool:
        """Check if ALL candidate actions have been tried for this state."""
        for c in candidates:
            action_type = c.get("action_type", "click")
            count = self.count_action(state_id, action_type,
                                      c["x"], c["y"], coord_tolerance)
            if count == 0:
                return False
        return True

    def penalty(self, state_id: str, action_type: str, x: int, y: int,
                coord_tolerance: int = 30) -> float:
        """Calculate penalty score (0.0 = never tried, higher = tried more)."""
        count = self.count_action(state_id, action_type, x, y, coord_tolerance)
        return count * config.history_penalty_weight

    def clear(self):
        self._history.clear()

    def __len__(self) -> int:
        return len(self._history)
