"""Keep the integration examples used by C++ tests identical to the handoff document."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ImuContractTest(unittest.TestCase):
    def test_published_examples_match_test_contract(self):
        text = (ROOT / "docs/IMU原始与解析数据接口.md").read_text(encoding="utf-8")
        examples = [json.loads(block) for block in re.findall(r"```json\s*\n(.*?)\n```", text, re.S)]
        fixture = json.loads((ROOT / "tests/fixtures/bmi088_contract.json").read_text(encoding="utf-8"))
        self.assertEqual(examples[:3], [fixture[name] for name in ["realtime", "snapshot", "pair"]])


if __name__ == "__main__":
    unittest.main()
