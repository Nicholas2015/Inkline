"""Run with python tests/search_worker.py [path/to/Inkline executable]."""

import argparse
import json
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT = ROOT / "dist" / "Inkline" / "Inkline.exe"
EXECUTABLE = DEFAULT


class SearchWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = pathlib.Path(self.temp.name)
        self.source = self.directory / "folder"
        self.source.mkdir()

    def run_worker(self, **request):
        path = self.directory / "request.json"
        path.write_text(json.dumps(request), encoding="utf-8")
        subprocess.run(
            [EXECUTABLE, "--search-worker", path], timeout=20, check=True
        )
        return json.loads((self.directory / "reply.json").read_bytes())

    def document(self, name, text, index=0):
        (self.directory / name).write_bytes(text.encode("utf-8"))
        return dict(
            id=index, snapshot=name, label=name, lo=0, hi=len(text.encode("utf-16-le")) // 2
        )

    def page(self, number):
        data = (self.directory / f"{number}.jsonl").read_bytes()
        return [json.loads(line) for line in data.splitlines()]

    def test_unicode_replace_and_navigation(self):
        doc = self.document("one", "🎨hey 123\nhey 456")
        options = dict(docs=[doc], query=r"(hey) (\d+)", regex=True, case=True)
        answer = self.run_worker(op="all", replacement=r"\2:\1", **options)
        self.assertEqual(answer["value"], "🎨123:hey\n456:hey")
        self.assertEqual(answer["count"], 2)
        answer = self.run_worker(op="next", left=0, right=0, **options)
        self.assertEqual((answer["a"], answer["b"]), (2, 9))
        answer = self.run_worker(op="prev", left=0, right=0, **options)
        self.assertEqual((answer["a"], answer["b"]), (10, 17))
        doc.update(lo=10)
        answer = self.run_worker(op="count", **options)
        self.assertEqual(answer["count"], 1)

    def test_multiple_tabs_and_pagination(self):
        docs = [self.document("first", "a"*512), self.document("second", "🎨a", 1)]
        self.assertEqual(self.run_worker(docs=docs, op="list", query="a")["count"], 513)
        self.assertEqual(len(self.page(0)), 512)
        self.assertEqual(self.page(1), [["second", 1, 2, 3, "a", 1]])

    def test_zero_width_and_errors(self):
        doc = self.document("text", "aaa")
        answer = self.run_worker(docs=[doc], op="next", query="(?=a)", regex=True,
                                 left=0, right=0, empty=0)
        self.assertEqual((answer["a"], answer["b"]), (1, 1))
        self.assertIn("error", self.run_worker(docs=[doc], op="count", query="(", regex=True))
        self.assertIn("error", self.run_worker(docs=[doc], op="all", query="a", regex=True,
                                               replacement=r"\3"))

    def test_folder_search_and_quick_open(self):
        path = self.source / "data.txt"
        path.write_bytes("甲\r\n乙 needle\n".encode("utf-16"))
        options = dict(root=str(self.source), glob="*.txt")
        result = self.run_worker(op="folder", query="needle", **options)
        self.assertEqual(result["count"], 1)
        row, = self.page(0)
        self.assertEqual(row[1:4], [2, 2, 8])
        result = self.run_worker(op="names", query="DATA", **options)
        self.assertEqual(result["count"], 1)
        self.assertEqual(self.page(0)[0][0], "data.txt")

    def test_gbk_selected_logs_report_byte_offsets(self):
        sources = []
        for name in ["one.log", "two.log"]:
            path = self.source / name
            path.write_bytes("甲乙\r\n文本目标\n".encode("gbk"))
            sources.append(str(path))
        result = self.run_worker(op="folder", root=str(self.source), files=sources,
                                 query="目标", codec="GBK")
        self.assertEqual(result["count"], 2)
        for row in self.page(0):
            self.assertEqual((row[1], row[3], row[6]), (2, 2, 10))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", type=pathlib.Path, default=DEFAULT)
    EXECUTABLE = parser.parse_args().executable.resolve()
    unittest.main(argv=[__file__])
