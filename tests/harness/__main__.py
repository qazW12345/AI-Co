"""Allow running the harness as: python -m tests.harness"""

from __future__ import annotations

import sys

from .runner import main

if __name__ == "__main__":
    sys.exit(main())
