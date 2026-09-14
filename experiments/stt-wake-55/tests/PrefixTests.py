import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from prefix import extract


class ExactWakePrefixTests(unittest.TestCase):
    def test_wake_only_and_whitespace(self):
        self.assertEqual(extract(" \tФАЙРФЛАЙ\n", "файрфлай"), dict(
            wake_detected=True, accepted_prefix="файрфлай", command_suffix=""))

    def test_one_utterance(self):
        self.assertEqual(extract("Файерфлай\t Включи  свет", "файерфлай")["command_suffix"], "включи свет")

    def test_no_false_prefixes(self):
        for text in ("я сказал файрфлай включи свет", "файрфлайка включи свет", "суперфайрфлай",
                     "файерфлай включи свет", "файр флай включи свет", "фаерфлай", "firefly",
                     "файрфлай-включи свет", "файрфлай, включи свет", '"файрфлай"', "", "  "):
            with self.subTest(text=text):
                self.assertEqual(extract(text, "файрфлай"), dict(
                    wake_detected=False, accepted_prefix=None, command_suffix=None))

    def test_failure_cannot_replay_previous_suffix(self):
        extract("файрфлай открой входную дверь", "файрфлай")
        self.assertIsNone(extract("открой входную дверь", "файрфлай")["command_suffix"])

    def test_forms_are_separate(self):
        for form in ("файрфлай", "файерфлай"):
            self.assertTrue(extract(form, form)["wake_detected"])
        self.assertFalse(extract("файрфлай", "файерфлай")["wake_detected"])
        with self.assertRaises(ValueError):
            extract("файр флай", "файр флай")


if __name__ == "__main__":
    unittest.main()
