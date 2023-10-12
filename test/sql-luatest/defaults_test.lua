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

g.test_only_numbers_with_signs = function()
    g.server:exec(function()
        -- Make sure STRING literal with '-' before it cannot be the default.
        local sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -'a');]]
        local res, err = box.execute(sql)
        local exp = [[Syntax error at line 1 near ''a'']]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure STRING literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +'a');]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure NULL literal with '-' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -NULL);]]
        res, err = box.execute(sql)
        exp = [[At line 1 at or near position 50: keyword 'NULL' is ]]..
              [[reserved. Please use double quotes if 'NULL' is an identifier.]]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure NULL literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +NULL);]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure UNKNOWN literal with '-' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -unknown);]]
        res, err = box.execute(sql)
        exp = [[At line 1 at or near position 50: keyword 'unknown' is ]]..
              [[reserved. Please use double quotes if 'unknown' is an ]]..
              [[identifier.]]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure UNKNOWN literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +unknown);]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure VARBINARY literal with '-' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -x'aa');]]
        res, err = box.execute(sql)
        exp = [[Syntax error at line 1 near 'x'aa'']]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure VARBINARY literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +x'aa');]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure TRUE literal with '-' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -true);]]
        res, err = box.execute(sql)
        exp = [[At line 1 at or near position 50: keyword 'true' is ]]..
              [[reserved. Please use double quotes if 'true' is an identifier.]]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure TRUE literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +true);]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure FALSE literal with '-' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -false);]]
        res, err = box.execute(sql)
        exp = [[At line 1 at or near position 50: keyword 'false' is ]]..
              [[reserved. Please use double quotes if 'false' is an ]]..
              [[identifier.]]
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure FALSE literal with '+' before it cannot be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +false);]]
        res, err = box.execute(sql)
        t.assert(res == nil);
        t.assert_equals(err.message, exp)

        -- Make sure INTEGER literal with '-' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -1);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{-1}})
        box.execute([[DROP TABLE t;]])

        -- Make sure INTEGER literal with '+' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +1);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{1}})
        box.execute([[DROP TABLE t;]])

        -- Make sure DOUBLE literal with '-' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -1.1e0);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{-1.1}})
        box.execute([[DROP TABLE t;]])

        -- Make sure DOUBLE literal with '+' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +1.1e0);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{1.1}})
        box.execute([[DROP TABLE t;]])

        -- Make sure DECIMAL literal with '-' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT -1.1);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{-1.1}})
        box.execute([[DROP TABLE t;]])

        -- Make sure DECIMAL literal with '+' before it can be the default.
        sql = [[CREATE TABLE t(i INT PRIMARY KEY, a ANY DEFAULT +1.1);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        box.execute([[INSERT INTO t(i) VALUES (1);]])
        t.assert_equals(box.execute([[SELECT a FROM t;]]).rows, {{1.1}})
        box.execute([[DROP TABLE t;]])
    end)
end
