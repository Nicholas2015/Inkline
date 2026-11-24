"""Run with the native GUI executable as the sole argument."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / "fixture.log"
        output = source.with_suffix(".out")

        def run(kind, **options):
            request = dict(path=str(source), kind=kind, size=source.stat().st_size,
                           start=0, codec="UTF-8", filter=True, regex=False,
                           case=True, query="ERROR")
            request.update(options)
            proc = subprocess.run([str(executable), "--log-job"], input=json.dumps(request).encode(),
                                  capture_output=True, timeout=20, check=True)
            result = json.loads(proc.stdout)
            assert "error" not in result, result
            return result

        source.write_bytes(b"zero\r\nerror one\r\nline two\r\nERROR xxx\ntrailing")
        matches = run("search", regex=True, case=False, query="^error")
        assert [hit[:2] for hit in matches["hits"]] == [[6, 5], [27, 5]]
        assert matches["resume"] == 37
        assert run("line", line=3)["offset"] == 17
        run("all", output=str(output), context=1, regex=True, case=False, query="^error")
        assert output.read_bytes() == source.read_bytes()
        run("batch", output=str(output), hits=[6, 8, 27])
        assert output.read_bytes() == b"error one\r\nERROR xxx\n"
        source.write_bytes(b"ABC\n" * 240)
        first = run("search", query="ABC", filter=False)
        rest = run("search", query="ABC", filter=False, start=first["more"])
        assert len(first["hits"]) == 200 and len(rest["hits"]) == 40 and first["more"] == 799
        source.write_bytes(b"x" * (128 * 1024 - 1) + "中文测试".encode() + b"\r\nfinish")
        for filter_lines in (False, True):
            result = run("search", query="中文", filter=filter_lines)
            assert result["hits"][0][:2] == [128 * 1024 - 1, 2]
        source.write_bytes("乗A\r\n匹配行\n".encode("gbk"))
        assert [hit[0] for hit in run("search", query="A", codec="GBK", filter=False)["hits"]] == [2]
        assert run("search", query="匹配", codec="GBK")["hits"][0][:2] == [5, 2]
        source.write_bytes(b"x" * 1048576 + b"Needle\r\n")
        assert run("search", query="needle", case=False)["hits"][0][0] == 1048576
        source.write_bytes(b"A\n" * 12000)
        index = run("line", line=7000)
        assert index["lines"] == 12001 and index["offset"] == 13998
        assert index["anchors"] == list(range(0, 24001, 2048))
        cached = run("line", line=12001, anchor=index["anchors"][12000 // 1024])
        assert cached == {"offset": 24000}
        source.write_bytes(b"a"* (128*1024-1) + b"\r\nb\rc\nd")
        assert run("line",line=2)["offset"] == 128*1024+1
        assert run("line",line=3)["offset"] == 128*1024+3
        assert run("line",line=4)["offset"] == 128*1024+5
        print("Native log worker regression tests passed.")

if __name__ == "__main__":
    main()

