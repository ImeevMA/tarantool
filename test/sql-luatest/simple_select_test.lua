local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
    g.server:exec(function()
        box.execute([[CREATE TABLE t1 (id INTEGER PRIMARY KEY, a STRING, b DOUBLE);]])
        box.execute([[CREATE TABLE t2 (id INTEGER PRIMARY KEY AUTOINCREMENT, "A" STRING);]])
        box.execute([[CREATE VIEW v1 AS SELECT id FROM t1;]])
        box.execute([[INSERT INTO t1 VALUES (1, 'abc', 1.5), (2, 'def', -3.0);]])
        box.execute([[INSERT INTO t2 VALUES (1, 'q');]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

-- Simple SELECT statements, which consist of a single source and result
-- columns referencing only fields of that source, are coded without the
-- generic expand and resolve machinery. These tests make sure that such
-- statements are executed, described and reported on error exactly as
-- the generic machinery does.

g.test_select_column = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local res = box.execute([[SELECT a FROM t1;]])
        t.assert_equals(res.metadata, {{name = 'a', type = 'string'}})
        t.assert_equals(res.rows, {{'abc'}, {'def'}})

        res = box.execute([[SELECT ALL a FROM t1;]])
        t.assert_equals(res.metadata, {{name = 'a', type = 'string'}})
        t.assert_equals(res.rows, {{'abc'}, {'def'}})
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_qualified_column = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local res = box.execute([[SELECT t1.a FROM t1;]])
        t.assert_equals(res.metadata, {{name = 'a', type = 'string'}})
        t.assert_equals(res.rows, {{'abc'}, {'def'}})

        res = box.execute([[SELECT x.a, x.b AS y FROM t1 AS x;]])
        t.assert_equals(res.metadata, {{name = 'a', type = 'string'},
                                       {name = 'y', type = 'double'}})
        t.assert_equals(res.rows, {{'abc', 1.5}, {'def', -3.0}})
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_asterisk = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local exp_meta = {{name = 'id', type = 'integer'},
                          {name = 'a', type = 'string'},
                          {name = 'b', type = 'double'}}
        local exp_rows = {{1, 'abc', 1.5}, {2, 'def', -3.0}}
        local res = box.execute([[SELECT * FROM t1;]])
        t.assert_equals(res.metadata, exp_meta)
        t.assert_equals(res.rows, exp_rows)

        res = box.execute([[SELECT t1.* FROM t1;]])
        t.assert_equals(res.metadata, exp_meta)
        t.assert_equals(res.rows, exp_rows)

        res = box.execute([[SELECT x.* FROM t1 AS x;]])
        t.assert_equals(res.metadata, exp_meta)
        t.assert_equals(res.rows, exp_rows)
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_duplicated_columns = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local res = box.execute([[SELECT id, a, id, b, a FROM t1;]])
        t.assert_equals(res.rows, {{1, 'abc', 1, 1.5, 'abc'},
                                   {2, 'def', 2, -3.0, 'def'}})
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_legacy = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        -- Statements which are not simple are coded by the generic
        -- machinery.
        local res = box.execute([[SELECT a FROM t1 WHERE id > 1;]])
        t.assert_equals(res.rows, {{'def'}})

        res = box.execute([[SELECT id FROM v1;]])
        t.assert_equals(res.rows, {{1}, {2}})

        res = box.execute([[SELECT a FROM t1 UNION SELECT "A" FROM t2;]])
        t.assert_equals(res.rows, {{'abc'}, {'def'}, {'q'}})
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_reverse_scan = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[SET SESSION "sql_reverse_unordered_selects" = true;]])
        local res = box.execute([[SELECT id FROM t1;]])
        t.assert_equals(res.rows, {{2}, {1}})
        box.execute([[SET SESSION "sql_reverse_unordered_selects" = false;]])
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_full_column_names = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[SET SESSION "sql_full_column_names" = true;]])
        local res = box.execute([[SELECT a FROM t1;]])
        t.assert_equals(res.metadata, {{name = 't1.a', type = 'string'}})

        res = box.execute([[SELECT x.* FROM t1 AS x;]])
        t.assert_equals(res.metadata, {{name = 't1.id', type = 'integer'},
                                       {name = 't1.a', type = 'string'},
                                       {name = 't1.b', type = 'double'}})
        box.execute([[SET SESSION "sql_full_column_names" = false;]])
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_full_metadata = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[SET SESSION "sql_full_metadata" = true;]])
        local res = box.execute([[SELECT * FROM t2;]])
        t.assert_equals(res.metadata, {
            {name = 'id', type = 'integer', is_nullable = false,
             is_autoincrement = true, span = 'id'},
            {name = 'A', type = 'string', is_nullable = true,
             span = 'A'},
        })
        box.execute([[SET SESSION "sql_full_metadata" = false;]])
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end

g.test_select_explain = function()
    g.server:exec(function()
        local res = box.execute([[EXPLAIN SELECT a FROM t1;]])
        local ops = {}
        for _, row in ipairs(res.rows) do
            ops[#ops + 1] = row[2]
        end
        t.assert_equals(ops, {'Init', 'OpenSpace', 'IteratorOpen', 'Explain',
                              'Rewind', 'Column', 'ResultRow', 'Next', 'Halt'})
        t.assert_equals(res.rows[4][6], 'SCAN TABLE t1 (~1048576 rows)')
    end)
end

g.test_select_seq_scan_error = function()
    g.server:exec(function()
        local _, err = box.execute([[SELECT a FROM t1;]])
        t.assert_equals(err.message, "Scanning is not allowed for 't1'")
    end)
end

g.test_select_errors = function()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        local _, err = box.execute([[SELECT nosuch FROM t1;]])
        t.assert_equals(err.message, "Can't resolve field 'nosuch'")

        _, err = box.execute([[SELECT t1.nosuch FROM t1;]])
        t.assert_equals(err.message,
                        "Field 'nosuch' was not found in space 't1' format")

        _, err = box.execute([[SELECT nosuch.a FROM t1;]])
        t.assert_equals(err.message,
                        "Field 'a' was not found in space 'nosuch' format")

        _, err = box.execute([[SELECT nosuch.* FROM t1;]])
        t.assert_equals(err.message, "Space 'nosuch' does not exist")

        _, err = box.execute([[SELECT a FROM nosuch;]])
        t.assert_equals(err.message, "Space 'nosuch' does not exist")
        box.execute([[SET SESSION "sql_seq_scan" = false;]])
    end)
end
