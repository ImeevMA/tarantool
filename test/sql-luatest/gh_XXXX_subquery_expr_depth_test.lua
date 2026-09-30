local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'subquery-expr-depth'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

--
-- Make sure the expressions of a subquery are not counted twice towards the
-- expression tree depth limit: once in the expression containing the subquery
-- and once more on their own.
--
g.test_subquery_expr_depth = function()
    g.server:exec(function()
        local function chain(n)
            return '1' .. string.rep(' + 0', n - 1)
        end
        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        box.execute([[INSERT INTO t VALUES (1);]])

        local res, err = box.execute('SELECT (SELECT ' .. chain(199) ..
                                     ') FROM SEQSCAN t;')
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {{1}})

        res, err = box.execute('SELECT i FROM SEQSCAN t WHERE i IN ' ..
                               '(SELECT ' .. chain(199) .. ');')
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {{1}})

        res, err = box.execute('SELECT EXISTS (SELECT 1 FROM SEQSCAN t ' ..
                               'LIMIT ' .. chain(199) .. ');')
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {{true}})

        res, err = box.execute('SELECT (SELECT (SELECT ' .. chain(198) ..
                               ') FROM SEQSCAN t) FROM SEQSCAN t;')
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {{1}})

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        res, err = box.execute('SELECT (SELECT ' .. chain(200) ..
                               ') FROM SEQSCAN t;')
        t.assert_equals(res, nil)
        t.assert_equals(err.message, exp_err)
        res, err = box.execute('SELECT (SELECT (SELECT ' .. chain(199) ..
                               ') FROM SEQSCAN t) FROM SEQSCAN t;')
        t.assert_equals(res, nil)
        t.assert_equals(err.message, exp_err)

        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure a result column alias is counted towards the expression tree depth
-- limit with the height of the expression it refers to.
--
g.test_alias_expr_depth = function()
    g.server:exec(function()
        local function chain(n)
            return '1' .. string.rep(' + 0', n - 1)
        end
        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        box.execute([[INSERT INTO t VALUES (1);]])

        local function check(n, clause, exp_depth)
            local sql = 'SELECT ' .. chain(n) .. ' AS a FROM SEQSCAN t ' ..
                        clause .. ';'
            local res, err = box.execute(sql)
            if exp_depth == nil then
                t.assert_equals(err, nil)
                t.assert_equals(res.rows, {{1}})
                return
            end
            t.assert_equals(res, nil)
            t.assert_equals(err.message, 'Number of nodes in expression ' ..
                            'tree ' .. exp_depth .. ' exceeds the limit (200)')
        end

        check(198, 'WHERE a + 0 = 1')
        check(199, 'WHERE a + 0 = 1', 201)
        check(199, 'WHERE (SELECT a) = 1', 201)
        check(199, 'ORDER BY a + 0')
        check(200, 'ORDER BY a + 0', 201)
        check(200, 'ORDER BY a')
        check(200, 'ORDER BY 1')
        check(200, 'ORDER BY a COLLATE "binary"', 201)
        check(199, 'GROUP BY a + 0')
        check(200, 'GROUP BY a + 0', 201)

        box.execute([[DROP TABLE t;]])
    end)
end
