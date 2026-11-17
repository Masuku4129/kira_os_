import unittest

from ci.latency_check import check, parse

SAMPLE = """\
[LAT] время реакции с последнего сброса (ядер 2, замер секций с выключенными прерываниями: включён)
[LAT] irq: n=17334 avg=293156 p50=114688 p99=3670016 p99.9=10485760 max=13310204 over=9 limit=10000000 нс, максимум: K+0x2d9c8
[LAT] sched-realtime: n=0 over=0 limit=50000000 нс
[LAT] sched-critical: n=7883 avg=89575 p50=32768 p99=2097152 p99.9=6291456 max=12593479 over=0 limit=50000000 нс, максимум: поток #1
"""


class LatencyCheckTests(unittest.TestCase):
    def test_parse(self):
        m = parse(SAMPLE)
        self.assertEqual(m["irq"]["p99.9"], 10485760)
        self.assertEqual(m["irq"]["over"], 9)
        self.assertEqual(m["sched-realtime"], {"n": 0, "over": 0, "limit": 50000000})
        self.assertEqual(m["sched-critical"]["max"], 12593479)

    def test_within_bounds(self):
        pol = {"bounds": {"irq": {"p99.9": 20000000, "min_n": 1000}, "sched-critical": {"p99": 3000000}}}
        self.assertEqual(check(parse(SAMPLE), pol), [])

    def test_regression_fails(self):
        # регресс: p99.9 прерывания выше границы - проверка обязана упасть
        pol = {"bounds": {"irq": {"p99.9": 5000000}}}
        errs = check(parse(SAMPLE), pol)
        self.assertEqual(len(errs), 1)
        self.assertIn("p99.9", errs[0])

    def test_max_boundary_is_inclusive(self):
        pol = {"bounds": {"irq": {"max": 13310204}}}
        self.assertEqual(check(parse(SAMPLE), pol), [])
        pol = {"bounds": {"irq": {"max": 13310203}}}
        self.assertEqual(len(check(parse(SAMPLE), pol)), 1)

    def test_not_measured_fails(self):
        # вид без замков и вид, которого нет в выводе, - провал: молчаливое «0 замеров» не должно проходить
        self.assertEqual(len(check(parse(SAMPLE), {"bounds": {"sched-realtime": {"p99": 1}}})), 1)
        self.assertEqual(len(check(parse(SAMPLE), {"bounds": {"isr": {"p99": 1}}})), 1)
        self.assertEqual(len(check(parse(SAMPLE), {"bounds": {"irq": {"p99": 10**12, "min_n": 10**6}}})), 1)


if __name__ == "__main__":
    unittest.main()
