from dataclasses import dataclass, field


@dataclass
class Config:
    # C backend (scrcpy internal API)
    scrcpy_host: str = "127.0.0.1"
    scrcpy_port: int = 18080

    # Python web server
    web_port: int = 8080

    # LLM / VLM
    api_key: str = ""
    model: str = "openai/gpt-4o-mini"
    vision_model: str = "google/gemini-2.5-flash-lite"
    base_url: str = "https://openrouter.ai/api/v1"

    # CLIP
    clip_model: str = "ViT-B-32"
    clip_pretrained: str = "laion2b_s34b_b79k"
    clip_sim_threshold: float = 0.7
    clip_screen_change_threshold: float = 0.95

    # Recording
    record_dir: str = ""  # set in post_init

    # Memory DB
    db_dir: str = ""  # set in post_init

    # Hybrid decision
    phash_threshold: int = 8        # pHash hamming distance threshold (< means same screen)
    memory_sim_threshold: float = 0.95  # ChromaDB similarity threshold for memory match
    history_window_size: int = 15   # ActionHistoryWindow deque size
    history_penalty_weight: float = 0.5  # penalty multiplier for recently-used actions

    # Guardrails
    max_same_screen: int = 3
    max_repeat_touch: int = 4
    max_auto_runtime_minutes: int = 120

    @property
    def scrcpy_url(self) -> str:
        return f"http://{self.scrcpy_host}:{self.scrcpy_port}"

    def __post_init__(self):
        import os
        if not self.record_dir:
            self.record_dir = os.path.expanduser("~/scrcpy_records")
        if not self.db_dir:
            self.db_dir = os.path.expanduser("~/scrcpy_db")


config = Config()
