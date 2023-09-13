local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

g.test_transaction_statements = function()
    g.server:exec(function()
        local exp = {row_count = 0}
        t.assert_equals(box.execute([[START TRANSACTION;]]), exp)
        t.assert_equals(box.execute([[SAVEPOINT ASD;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK TO Asd;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK TO "ASD";]]), exp)
        local res, err = box.execute([[ROLLBACK TO "aSd";]])
        t.assert_equals(err.message, "Can not rollback to savepoint: "..
                        "the savepoint does not exist")
        t.assert_equals(box.execute([[RELEASE asD;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK;]]), exp)
    end)
end

g.test_collate_clause = function()
    g.server:exec(function()
        local coll = {"ASD", 1, "BINARY", "asdloc", {strength = "primary"}}
        local coll_id = box.space._collation:auto_increment(coll).id
        local sql = [[CREATE TABLE t(s STRING PRIMARY KEY COLLATE aSd);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        t.assert_equals(box.space.t:format()[1].collation, coll_id)
        t.assert_equals(box.space.t.index[0].parts[1].collation, "ASD")
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t(i INT PRIMARY KEY, s STRING);]])
        sql = [[CREATE INDEX i ON t(s COLLATE asd)]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        t.assert_equals(box.space.t.index[1].parts[1].collation, "ASD")
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t(i INT PRIMARY KEY, s STRING);]])
        sql = [[SELECT s FROM t WHERE s COLLATE Asd == '1';]]
        t.assert_equals(box.execute(sql).rows, {})
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t(s STRING PRIMARY KEY COLLATE "aSd");]]
        local res, err = box.execute(sql)
        t.assert_equals(err.message, "Collation 'aSd' does not exist")

        box.execute([[CREATE TABLE t(i INT PRIMARY KEY, s STRING);]])
        sql = [[CREATE INDEX i ON t(s COLLATE "asd")]]
        res, err = box.execute(sql)
        t.assert_equals(err.message, "Collation 'asd' does not exist")
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t(i INT PRIMARY KEY, s STRING);]])
        sql = [[SELECT s FROM t WHERE s COLLATE "Asd" == '1';]]
        res, err = box.execute(sql)
        t.assert_equals(err.message, "Collation 'Asd' does not exist")
        box.execute([[DROP TABLE t;]])

        box.space._collation:delete({coll_id})
    end)
end
