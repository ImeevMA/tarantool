-- Regression test for #2251
test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
box.execute([[SET SESSION "sql_seq_scan" = true;]])

-- box.cfg()

box.execute("CREATE TABLE T1(A INTEGER PRIMARY KEY, B INT UNIQUE, E INT);")
box.execute("INSERT INTO t1 VALUES(1,4,6);")
box.execute("INSERT INTO t1 VALUES(2,5,7);")

box.execute("UPDATE t1 SET e=e+1 WHERE b IN (SELECT b FROM t1);")

box.execute("SELECT e FROM t1")

box.execute("CREATE TABLE T2(A INTEGER PRIMARY KEY, B INT UNIQUE, C NUMBER, "..\
            "D NUMBER, E INT,  UNIQUE(C,D));")
box.execute("INSERT INTO t2 VALUES(1,2,3,4,5);")
box.execute("INSERT INTO t2 VALUES(2,3,4,4,6);")

box.execute("UPDATE t2 SET e=e+1 WHERE b IN (SELECT b FROM t2);")

box.execute("SELECT e FROM t2")

box.execute("DROP TABLE t1")
box.execute("DROP TABLE t2")

-- Debug
-- require("console").start()
