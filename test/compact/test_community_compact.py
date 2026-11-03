"""Isolated community COMPACT integration tests (requires taospy).

TDENGINE_BUILD_DIR=/path/to/build LD_LIBRARY_PATH=/path/to/build/build/lib \
  python test/compact/test_community_compact.py -v

Only a temporary data directory and a private loopback port are used.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import tempfile
import time
import unittest

import taos

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("TDENGINE_BUILD_DIR", ROOT / "debug"))
DAY = 86_400_000
BASE = (int(time.time() * 1000) // DAY - 4) * DAY


def eventually(check, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(f"condition timed out after {timeout}s")


class Server:
    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="tdengine-compact-"))
        self.cfg = self.root / "cfg"
        self.cfg.mkdir()
        for _ in range(100):
            self.port = random.randint(18000, 28000)
            sockets = []
            try:
                for offset in range(6):
                    s = socket.socket()
                    sockets.append(s)
                    s.bind(("127.0.0.1", self.port + offset))
                break
            except OSError:
                continue
            finally:
                for s in sockets:
                    s.close()
        else:
            raise RuntimeError("no private port range available")
        (self.cfg / "taos.cfg").write_text(
            f"fqdn localhost\nserverPort {self.port}\nfirstEp localhost:{self.port}\n"
            f"dataDir {self.root}/data\nlogDir {self.root}/log\n"
            "monitor 0\ntelemetryReporting 0\nsupportVnodes 32\n"
            "numOfRpcThreads 2\nnumOfRpcSessions 100\nnumOfTaskQueueThreads 4\n"
            "numOfCompactThreads 1\ncompactSpeedLimitMB 1\ncompactPullupInterval 1\n"
        )
        self.process = None
        self.output = None
        self.conn = None

    def connect(self):
        return taos.connect(host="localhost", port=self.port, user="root", password="taosdata", config=str(self.cfg))

    def start(self):
        self.output = (self.root / "stdout.log").open("ab")
        self.process = subprocess.Popen([str(BUILD / "build/bin/taosd"), "-c", str(self.cfg)],
                                        stdout=self.output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(f"taosd exited: {self.root / 'stdout.log'}")
            try:
                self.conn = self.connect()
                self.sql("select server_version()")
                eventually(lambda: any("ready" in row for row in self.sql("show dnodes")), timeout=30)
                return
            except Exception:
                time.sleep(0.2)
        raise AssertionError(f"taosd did not start: {self.root}")

    def stop(self, crash=False):
        if self.conn:
            self.conn.close()
            self.conn = None
        if self.process and self.process.poll() is None:
            self.process.kill() if crash else self.process.terminate()
            try:
                self.process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
                if not crash:
                    raise AssertionError("graceful shutdown timed out")
        if self.output:
            self.output.close()

    def sql(self, sql, conn=None):
        result = (conn or self.conn).query(sql)
        return result.fetch_all() if result.field_count else []

    def manifest(self, db):
        result = {}
        for vg in self.sql(f"show {db}.vgroups"):
            path = self.root / f"data/vnode/vnode{vg[0]}/tsdb/current.json"
            if path.exists():
                for fset in json.loads(path.read_text())["fset"]:
                    result[(vg[0], fset["fid"])] = fset
        return result

    def flush(self, db):
        before = self.manifest(db)
        self.sql(f"flush database {db}")
        eventually(lambda: self.manifest(db) != before)

    def compact(self, sql):
        rows = self.sql(sql)
        assert rows[0][0] == "accepted", rows
        return rows[0][1]

    def wait(self, compact_id):
        eventually(lambda: not any(row[0] == compact_id for row in self.sql("show compacts")))

    def files_size(self, db):
        total = 0
        for (vg, _), fset in self.manifest(db).items():
            for kind in ("head", "data", "sma", "tomb"):
                if kind in fset:
                    total += fset[kind]["size"]
            for level in fset["stt lvl"]:
                total += sum(f["size"] for f in level["files"])
        return total

    def insert(self, db, table, start, count, generation=0, payload=False):
        for offset in range(0, count, 500):
            rows = []
            for i in range(offset, min(offset + 500, count)):
                tail = f",'{hashlib.sha512(f'{generation}:{i}'.encode()).hexdigest()}'" if payload else ""
                rows.append(f"({start+i},{generation*10000+i}{tail})")
            self.sql(f"insert into {db}.{table} values " + " ".join(rows))


@unittest.skipUnless((BUILD / "build/bin/taosd").exists(), "set TDENGINE_BUILD_DIR to a compiled community build")
class CommunityCompactTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = Server()
        print(f"\nCOMPACT integration artifacts: {cls.server.root}", flush=True)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_01_reclaim_deleted_and_dropped_data(self):
        s = self.server
        for trigger in (1, 8):
            with self.subTest(stt_trigger=trigger):
                db = f"reclaim_{trigger}"
                s.sql(f"create database {db} vgroups 1 duration 1d buffer 16 stt_trigger {trigger}")
                for table in ("live", "dead"):
                    s.sql(f"create table {db}.{table} (ts timestamp, v int, payload binary(128))")
                for generation in range(3):
                    s.insert(db, "live", BASE, 4000, generation, payload=True)
                    s.flush(db)
                s.insert(db, "dead", BASE, 3000, payload=True)
                s.flush(db)
                s.sql(f"delete from {db}.live where ts <= {BASE+2999}")
                s.sql(f"drop table {db}.dead")
                s.flush(db)
                expected = s.sql(f"select * from {db}.live order by ts")
                self.assertEqual(len(expected), 1000)
                before = s.files_size(db)
                ident = s.compact(f"compact database {db}")
                s.wait(ident)
                self.assertEqual(s.sql(f"select * from {db}.live order by ts"), expected)
                self.assertLess(s.files_size(db), before * 0.65)
                self.assertTrue(all(f["last compact"] > 0 for f in s.manifest(db).values()))
                # A later insert at a deleted timestamp must remain visible.
                s.sql(f"insert into {db}.live values ({BASE},999,'reinsert')")
                s.flush(db)
                s.wait(s.compact(f"compact database {db}"))
                self.assertEqual(s.sql(f"select v from {db}.live where ts={BASE}"), [(999,)])

    def test_02_range_vgroups_force_and_validation(self):
        s = self.server
        db = "ranged"
        s.sql(f"create database {db} vgroups 2 duration 1d buffer 16 stt_trigger 8")
        for i in range(16):
            s.sql(f"create table {db}.t{i} (ts timestamp,v int)")
            s.insert(db, f"t{i}", BASE, 10)
            s.insert(db, f"t{i}", BASE + 2*DAY, 10)
        s.flush(db)
        before = s.manifest(db)
        self.assertEqual(len(before), 4)
        vg, fid = sorted(before)[0]
        s.wait(s.compact(f"compact {db}.vgroups in ({vg}) start with {BASE} end with {BASE+1000}"))
        after = s.manifest(db)
        self.assertGreater(after[(vg, fid)]["last compact"], 0)
        for key in before:
            if key != (vg, fid):
                self.assertEqual(before[key], after[key])
        cid = after[(vg, fid)]["last compact cid"]
        s.sql(f"use {db}")
        s.wait(s.compact(f"compact vgroups in ({vg}) start with {BASE} end with {BASE+1000}"))
        self.assertEqual(s.manifest(db)[(vg, fid)]["last compact cid"], cid)
        s.wait(s.compact(f"compact vgroups in ({vg}) start with {BASE} end with {BASE+1000} force"))
        self.assertGreater(s.manifest(db)[(vg, fid)]["last compact cid"], cid)
        s.sql("create database foreign_db vgroups 1 buffer 16")
        foreign = s.sql("show foreign_db.vgroups")[0][0]
        for sql in (f"compact {db}.vgroups in ({foreign})", f"compact {db}.vgroups in (2147483647)",
                    f"compact {db}.vgroups in ({vg},{vg})",
                    f"compact database {db} start with {BASE+1000} end with {BASE}"):
            with self.assertRaises(taos.Error):
                s.sql(sql)
        self.assertFalse(s.sql("show compacts"))

    def test_03_meta_only_and_schema_history(self):
        s = self.server
        db = "metadata"
        s.sql(f"create database {db} vgroups 1 buffer 16")
        s.sql(f"create stable {db}.st (ts timestamp,v int) tags (t1 int)")
        s.sql(f"create table {db}.ct using {db}.st tags (0)")
        s.insert(db, "ct", BASE, 100)
        s.flush(db)
        for i in range(1200):
            s.sql(f"alter table {db}.ct set tag t1={i}")
        for i in range(12):
            s.sql(f"alter stable {db}.st add column c{i} int")
        before = s.manifest(db)
        s.wait(s.compact(f"compact database {db} meta_only"))
        self.assertEqual(before, s.manifest(db))
        self.assertEqual(s.sql(f"select count(*) from {db}.st where t1=1199"), [(100,)])
        s.sql(f"insert into {db}.ct (ts,v,c11) values ({BASE+100},123,456)")
        s.wait(s.compact(f"compact database {db} force"))
        self.assertEqual(s.sql(f"select c11 from {db}.ct where ts={BASE+100}"), [(456,)])
        self.assertEqual(s.sql(f"select count(*) from {db}.ct"), [(101,)])
        s.stop()
        s.start()
        self.assertEqual(s.sql(f"select count(*) from {db}.st where t1=1199"), [(101,)])
        self.assertEqual(s.sql(f"select c11 from {db}.ct where ts={BASE+100}"), [(456,)])

    def test_04_wait_for_previous_compact(self):
        s = self.server
        s.sql("create database queued vgroups 1 buffer 16")
        first = s.compact("compact database queued")
        def second():
            conn = s.connect()
            try:
                return s.sql("compact database queued", conn)[0][1]
            finally:
                conn.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(second)
            time.sleep(0.3)
            self.assertFalse(pending.done())
            second_id = pending.result(timeout=90)
        self.assertNotEqual(first, second_id)
        s.wait(second_id)

    def test_05_kill_and_crash_recovery(self):
        s = self.server
        db = "recovering"
        s.sql(f"create database {db} vgroups 1 duration 1d buffer 16 stt_trigger 8")
        s.sql(f"create table {db}.t (ts timestamp,v int,payload binary(128))")
        for day in range(4):
            s.insert(db, "t", BASE + day*DAY, 15000, payload=True)
        s.flush(db)
        expected = s.sql(f"select count(*),sum(v) from {db}.t")
        ident = s.compact(f"compact database {db} force")
        s.sql(f"kill compact {ident}")
        s.wait(ident)
        self.assertEqual(s.sql(f"select count(*),sum(v) from {db}.t"), expected)
        ident = s.compact(f"compact database {db} force")
        states = list((s.root / "data/vnode").glob("vnode*/tsdb-compact.state"))
        self.assertTrue(any(path.read_bytes()[0] == 1 for path in states))
        s.stop(crash=True)
        s.start()
        s.wait(ident)
        self.assertEqual(s.sql(f"select count(*),sum(v) from {db}.t"), expected)
        # Completed state survives another restart.
        s.stop()
        s.start()
        self.assertFalse(s.sql("show compacts"))
        self.assertEqual(s.sql(f"select count(*),sum(v) from {db}.t"), expected)

    def test_06_primary_keys_delete_boundaries_and_precision(self):
        s = self.server
        for precision, scale in (("us", 1000), ("ns", 1000000)):
            with self.subTest(precision=precision):
                db = f"keys_{precision}"
                base = BASE * scale
                s.sql(f"create database {db} vgroups 1 buffer 16 precision '{precision}'")
                s.sql(f"create table {db}.t (ts timestamp,pk int primary key,v int)")
                values = " ".join(f"({base+i},{pk},{i*3+pk})" for i in range(10) for pk in range(3))
                s.sql(f"insert into {db}.t values {values}")
                s.flush(db)
                s.sql(f"delete from {db}.t where ts>={base+2} and ts<={base+5}")
                s.sql(f"insert into {db}.t values ({base+5},1,99)")
                s.flush(db)
                s.sql(f"delete from {db}.t where ts>={base+5} and ts<={base+7}")
                s.sql(f"insert into {db}.t values ({base+7},2,100)")
                expected = s.sql(f"select cast(ts as bigint),pk,v from {db}.t order by ts,pk")
                self.assertEqual(len(expected), 13)
                s.wait(s.compact(f"compact database {db} start with {base} end with {base+9} force"))
                self.assertEqual(s.sql(f"select cast(ts as bigint),pk,v from {db}.t order by ts,pk"), expected)
                s.sql(f"drop table {db}.t")
                s.wait(s.compact(f"compact database {db} force"))
                self.assertFalse(s.manifest(db))

    def test_07_concurrent_commit_is_not_skipped(self):
        s = self.server
        db = "concurrent_compact"
        s.sql(f"create database {db} vgroups 1 duration 1d buffer 16 stt_trigger 8")
        s.sql(f"create table {db}.t (ts timestamp,v int,payload binary(128))")
        s.insert(db, "t", BASE, 60000, payload=True)
        s.flush(db)
        vg = s.sql(f"show {db}.vgroups")[0][0]
        path = s.root / f"data/vnode/vnode{vg}/tsdb"
        old_heads = set(path.glob("*.head"))
        ident = s.compact(f"compact database {db} force")
        eventually(lambda: set(path.glob("*.head")) - old_heads)
        # A query and a new commit run while the rewrite owns its input snapshot.
        self.assertEqual(s.sql(f"select count(*) from {db}.t"), [(60000,)])
        s.insert(db, "t", BASE + 60000, 100, generation=2, payload=True)
        s.sql(f"delete from {db}.t where ts>={BASE+200} and ts<={BASE+299}")
        s.insert(db, "t", BASE + 200, 10, generation=4, payload=True)
        s.flush(db)
        expected = s.sql(f"select count(*),sum(v) from {db}.t")
        s.wait(ident)
        self.assertEqual(s.sql(f"select count(*),sum(v) from {db}.t"), expected)
        pending = s.manifest(db)
        self.assertTrue(any(f["cid"] != fs["last compact cid"] for fs in pending.values()
                            for lvl in fs["stt lvl"] for f in lvl["files"]))
        s.wait(s.compact(f"compact database {db}"))
        self.assertEqual(s.sql(f"select count(*),sum(v) from {db}.t"), expected)
        for fs in s.manifest(db).values():
            self.assertTrue(all(f["cid"] == fs["last compact cid"] for lvl in fs["stt lvl"] for f in lvl["files"]))

    def test_08_force_cleanup(self):
        s = self.server
        s.sql("create database force_cleanup vgroups 1 buffer 16")
        ident = s.compact("compact database force_cleanup")
        s.sql(f"kill compact {ident} force")
        s.wait(ident)


if __name__ == "__main__":
    unittest.main()
