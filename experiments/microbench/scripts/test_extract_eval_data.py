"""Regression tests for parsing libtiermem microbenchmark logs."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("extract_eval_data.py")


class LibtiermemLogTests(unittest.TestCase):
    def test_object_line_with_raw_store_rate(self):
        spec = importlib.util.spec_from_file_location("extract_eval_data", SCRIPT)
        extractor = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(extractor)

        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "stderr.log"
            log.write_text(
                "TIERMEM: === Epoch 0 (T=5.00s) ===\n"
                "TIERMEM:   iMC WR: node0=1.00 GB/s node1=2.00 GB/s\n"
                "TIERMEM:   iMC RD: node0=3.00 GB/s node1=4.00 GB/s\n"
                "TIERMEM:   obj 0x1234 (1048576 pages, node 0, pid 42): "
                "loads=10 reg_ld_raw=0.09 reg_ld=0.10 reg_st_raw=0.20 reg_st=0.30 "
                "dd_rd=1.00e+03 hw_dp=2.00e+03 sw_dp=3.00e+03 "
                "d_rfo=4.00e+03 hw_rp=5.00e+03 shl=6.00e+03 "
                "rd_bw=7.00 GB/s wr_bw=8.00 GB/s bw=15.00 GB/s\n"
            )
            epochs = extractor.parse_libtiermem(log)

        self.assertEqual(len(epochs), 1)
        self.assertEqual(epochs[0]["imc_rd"], (3.0, 4.0))
        self.assertEqual(len(epochs[0]["objs"]), 1)
        self.assertEqual(epochs[0]["objs"][0]["reg_st"], 0.30)
        self.assertEqual(epochs[0]["objs"][0]["bw"], 15.0)


if __name__ == "__main__":
    unittest.main()
