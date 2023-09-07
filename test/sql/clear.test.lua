test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
_ = box.space._session_settings:update('sql_default_engine', {{'=', 2, engine}})
box.execute([[SET SESSION "sql_seq_scan" = true;]])

-- box.cfg()

-- create space
box.execute("CREATE TABLE ZOOBAR (C1 INT, C2 INT PRIMARY KEY, C3 TEXT, C4 INT)")
box.execute("CREATE UNIQUE INDEX zoobar2 ON zoobar(c1, c4)")

-- Seed entry
for i=1, 100 do box.execute(string.format("INSERT INTO zoobar VALUES (%d, %d, 'c3', 444)", i+i, i)) end

-- Check table is not empty
box.execute("SELECT * FROM zoobar")

-- Do clean up
box.execute("DELETE FROM zoobar")

-- Make sure table is empty
box.execute("SELECT * from zoobar")

-- Cleanup
box.execute("DROP INDEX zoobar2 ON zoobar")
box.execute("DROP TABLE zoobar")

-- Debug
-- require("console").start()

--
-- gh-4183: Check if there is a garbage in case of failure to
-- create a constraint, when more than one constraint of the same
-- type is created with the same name and in the same
-- CREATE TABLE statement.
--
box.execute("CREATE TABLE T1(ID INT PRIMARY KEY, "..\
            "CONSTRAINT ck1 CHECK(ID > 0), CONSTRAINT ck1 CHECK(ID < 0));")
box.space.t1
