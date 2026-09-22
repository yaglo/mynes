"""Unit and integration tests for tools/showcase.

Run from the repository root:
  python3 -m unittest discover -s tools/showcase/tests -t tools/showcase -v
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
