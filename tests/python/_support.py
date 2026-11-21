from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase


class WorkspaceTest(TestCase):
    workspace: Path

    def setUp(self) -> None:
        super().setUp()
        scratch = Path(__file__).resolve().parents[2] / ".temp"
        scratch.mkdir(exist_ok=True)
        directory = TemporaryDirectory(prefix="test-", dir=scratch)
        self.addCleanup(directory.cleanup)
        self.workspace = Path(directory.name)
