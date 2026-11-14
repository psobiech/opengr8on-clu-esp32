import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]


class FirmwareReleaseTests(unittest.TestCase):
    def test_release_assets(self):
        workflow = (ROOT / ".github/workflows/firmware.yml").read_text()
        script = textwrap.dedent(workflow.split("      - name: Assets\n        run: |\n", 1)[1]
                                 .split("      - name: Publish", 1)[0])
        presets = json.loads((ROOT / "CMakePresets.json").read_text())["configurePresets"]
        with tempfile.TemporaryDirectory(dir=ROOT / "test") as directory:
            work = Path(directory)
            for preset in presets:
                name = preset["name"]
                artifact = work / "artifacts" / f"opengr8on-clu-{name}"
                artifact.mkdir(parents=True)
                for kind in ("full", "ota"):
                    (artifact / f"opengr8on-clu-{name}-{kind}.bin").write_text(f"{name}:{kind}")
            subprocess.run(["bash", "-c", script], cwd=work, check=True,
                           env={**os.environ, "GITHUB_REF_NAME": "v-test"},
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            assets = work / "assets"
            expected = set()
            for suffix in ("", "-N8R8"):
                name = "opengr8on-gate-http-esp32s3-v-test"
                for kind, marker in (("", "full"), ("-ota", "ota")):
                    filename = f"{name}{kind}{suffix}.bin"
                    expected.add(filename)
                    self.assertEqual((assets / filename).read_text(), f"gate-http{suffix}:{marker}")
                filename = f"{name}{suffix}.zip"
                expected.add(filename)
                with zipfile.ZipFile(assets / filename) as archive:
                    self.assertEqual(archive.read(f"firmware/opengr8on-clu-gate-http{suffix}-full.bin"),
                                     f"gate-http{suffix}:full".encode())
            for board in ("waveshare", "f8", "a8v3", "gate"):
                expected.add(f"opengr8on-clu-{board}-v-test.zip")
                if board != "gate":
                    for kind in ("", "-ota"):
                        for variant in ("", "-mysensors"):
                            expected.add(f"opengr8on-clu-{board}-v-test{kind}{variant}.bin")
            self.assertEqual({path.name for path in assets.iterdir()}, expected)


if __name__ == "__main__":
    unittest.main()