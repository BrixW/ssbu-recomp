#!/usr/bin/env python3
"""Unit tests of ladder.py: Glicko-2 against Glickman's worked example, identity (invitation, open sign-up, key proof,
names), a Bo3 with matching reports (both ratings change), disputed games (reports differ, a desync, one report only,
not the ladder's rules), draws, a set left half-played, and the web documents (no ids or keys).
    python -m unittest tools/net/test_ladder.py        (standard library only)"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ladder as LD  # noqa: E402


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def report(seed, slot, stocks=(2, 0), dmg=(30, 80), **kw):
    r = {"seed": seed, "slot": slot, "stage": 7, "chara": (0x11, 0x22), "stocks": stocks, "dmg": dmg, "frames": 9000,
         "rule_kind": LD.RULE_STOCK, "stock": LD.LADDER_STOCKS, "desyncs": 0, "flags": 0}
    r.update(kw)
    return r


class Glicko2Test(unittest.TestCase):
    def test_paper_example(self):
        r, rd, vol = LD.glicko2(1500, 200, 0.06, [(1400, 30, 1), (1550, 100, 0), (1700, 300, 0)])
        self.assertAlmostEqual(r, 1464.06, delta=0.01)   # (the paper rounds its intermediate steps: 1464.0507 exact)
        self.assertAlmostEqual(rd, 151.52, places=2)
        self.assertAlmostEqual(vol, 0.05999, delta=1e-5)

    def test_no_games_grows_rd(self):
        r, rd, vol = LD.glicko2(1500, 200, 0.06, [])
        self.assertEqual(r, 1500)
        self.assertGreater(rd, 200)

    def test_winner(self):
        self.assertEqual(LD.game_winner((1, 0), (200, 10)), 0)
        self.assertEqual(LD.game_winner((2, 2), (90, 40)), 1)
        self.assertIsNone(LD.game_winner((1, 1), (50, 50)))


class LadderTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.clock = Clock()
        self.ld = LD.Ladder(os.path.join(self.dir.name, "ladder.db"), now=self.clock)
        self.a, self.ka = os.urandom(16), os.urandom(32)
        self.b, self.kb = os.urandom(16), os.urandom(32)
        self.assertEqual(self.ld.identify(self.a, "x", key=self.ka, invite=self.ld.invite("Roberto")), (self.a, "Roberto"))
        self.assertEqual(self.ld.identify(self.b, "y", key=self.kb, invite=self.ld.invite("FreDy")), (self.b, "FreDy"))

    def tearDown(self):
        self.ld.db.close()
        self.dir.cleanup()

    def play(self, seed, winner_slot=0, stocks=None, **kw):
        stocks = stocks or ((2, 0) if winner_slot == 0 else (0, 1))
        self.assertIsNone(self.ld.report(self.a, self.b, report(seed, 0, stocks, **kw)))
        return self.ld.report(self.b, self.a, report(seed, 1, stocks, **kw))

    # identity
    def test_identity(self):
        nonce = os.urandom(8)
        self.assertEqual(self.ld.identify(self.a, "?", nonce, LD.join_proof(self.ka, nonce)), (self.a, "Roberto"))
        self.assertEqual(self.ld.identify(self.a, "?", nonce, LD.join_proof(self.kb, nonce))[1], "wrong key")
        c = os.urandom(16)
        self.assertEqual(self.ld.identify(c, "Mallory", key=os.urandom(32))[1], "not invited")
        self.ld.open_signup = True
        self.assertEqual(self.ld.identify(c, "roberto", key=os.urandom(32))[1], "name taken")
        self.assertEqual(self.ld.identify(c, "Mallory", key=os.urandom(32)), (c, "Mallory"))

    def test_invite_is_single_use(self):
        code = self.ld.invite("Third")
        self.assertEqual(self.ld.identify(os.urandom(16), "", key=os.urandom(32), invite=code)[1], "Third")
        self.assertEqual(self.ld.identify(os.urandom(16), "", key=os.urandom(32), invite=code)[1], "invalid invitation")

    # sets
    def test_bo3_changes_ratings(self):
        self.ld.open_set(self.a, self.b)
        self.assertEqual(self.play(1, 0), "ok")
        self.assertEqual(self.play(2, 1), "ok")
        self.assertIsNotNone(self.ld.open_set_of(self.a, self.b))
        self.assertEqual(self.play(3, 0), "ok")
        self.assertIsNone(self.ld.open_set_of(self.a, self.b))
        ra, rb = self.ld.player(self.a), self.ld.player(self.b)
        self.assertGreater(ra[1], 1500)
        self.assertLess(rb[1], 1500)
        self.assertEqual((ra[4], ra[5], rb[4], rb[5]), (1, 0, 0, 1))

    def test_2_0_closes_set(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 1)
        self.play(2, 1)
        self.assertGreater(self.ld.player(self.b)[1], 1500)

    def test_disputed(self):
        self.ld.open_set(self.a, self.b)
        self.ld.report(self.a, self.b, report(1, 0, (2, 0)))
        self.assertEqual(self.ld.report(self.b, self.a, report(1, 1, (0, 1))), "disputed: reports differ")
        self.assertEqual(self.play(2, desyncs=3), "disputed: desync")
        self.assertEqual(self.play(3, stock=2), "disputed: not ladder rules")
        self.assertEqual(self.play(4, flags=LD.F_DISCONNECT), "disputed: abandoned")
        self.ld.report(self.a, self.b, report(5, 0))
        self.clock.t += LD.REPORT_WAIT + 1
        self.assertEqual(self.ld.sweep(), ["disputed: one report only"])
        self.assertEqual(self.ld.open_set_of(self.a, self.b)[3:], (0, 0))
        self.assertEqual(self.ld.player(self.a)[1], 1500)

    def test_report_from_outsider_ignored(self):
        self.ld.open_set(self.a, self.b)
        self.ld.report(self.a, self.b, report(1, 0))
        self.assertIsNone(self.ld.report(os.urandom(16), self.a, report(1, 1)))

    def test_draw_and_stage(self):
        self.ld.open_set(self.a, self.b)
        self.assertEqual(self.play(1, stocks=(1, 1), dmg=(50, 50)), "draw")
        self.ld.legal_stages = {3}
        self.assertEqual(self.play(2), "disputed: stage not legal")

    def test_void_on_leave(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 0)
        self.assertEqual(self.ld.void_sets(self.b), 1)
        self.assertIsNone(self.ld.open_set_of(self.a, self.b))
        self.assertEqual(self.ld.player(self.a)[1], 1500)

    # web
    def test_web_documents(self):
        self.ld.open_set(self.a, self.b)
        self.play(1, 0)
        self.play(2, 0)
        t = self.ld.table()
        self.assertEqual([p["name"] for p in t], ["Roberto", "FreDy"])
        self.assertEqual(t[0]["main"], "%016x" % 0x11)
        h = self.ld.history("fredy")
        self.assertEqual(h["sets"][0]["opponent"], "Roberto")
        self.assertEqual(h["sets"][0]["score"], [0, 2])
        self.assertIs(h["sets"][0]["won"], False)
        doc = json.dumps([t, h])
        for secret in (self.a, self.b, self.ka, self.kb):
            self.assertNotIn(secret.hex(), doc)
        self.assertIsNone(self.ld.history("nobody"))


if __name__ == "__main__":
    unittest.main()
