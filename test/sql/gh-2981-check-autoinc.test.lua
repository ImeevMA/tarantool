test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})

box.cfg{}

box.execute("CREATE TABLE T1 (S1 INTEGER PRIMARY KEY AUTOINCREMENT, "..\
            "S2 INTEGER, CHECK (S1 <> 19));");
box.execute("CREATE TABLE T2 (S1 INTEGER PRIMARY KEY AUTOINCREMENT, "..\
            "S2 INTEGER, CHECK (S1 <> 19 AND S1 <> 25));");
box.execute("CREATE TABLE T3 (S1 INTEGER PRIMARY KEY AUTOINCREMENT, "..\
            "S2 INTEGER, CHECK (S1 < 10));");

box.execute("insert into t1 values (18, null);")
box.execute("insert into t1(s2) values (null);")

box.execute("insert into t2 values (18, null);")
box.execute("insert into t2(s2) values (null);")
box.execute("insert into t2 values (24, null);")
box.execute("insert into t2(s2) values (null);")

box.execute("insert into t3 values (9, null)")
box.execute("insert into t3(s2) values (null)")

box.execute("DROP TABLE t1")
box.execute("DROP TABLE t2")
box.execute("DROP TABLE t3")

box.func.check_T1_ck_unnamed_T1_1:drop()
box.func.check_T2_ck_unnamed_T2_1:drop()
box.func.check_T3_ck_unnamed_T3_1:drop()
