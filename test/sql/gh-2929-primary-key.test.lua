test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})

-- All tables in SQL are now WITHOUT ROW ID, so if user
-- tries to create table without a primary key, an appropriate error message
-- should be raised. This tests checks it.

box.cfg{}

box.execute("CREATE TABLE T1(A INT PRIMARY KEY, B INT UNIQUE)")
box.execute("CREATE TABLE T2(A INT UNIQUE, B INT)")

box.execute("CREATE TABLE T3(A NUMBER)")
box.execute("CREATE TABLE T4(A NUMBER, B TEXT)")
box.execute("CREATE TABLE T5(A NUMBER, B NUMBER UNIQUE)")

box.execute("DROP TABLE t1")

--
-- gh-3522: invalid primary key name
--
box.execute("CREATE TABLE TX (A INT, PRIMARY KEY (B));")
