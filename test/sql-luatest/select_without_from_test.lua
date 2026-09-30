local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'select-without-from'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

g.test_limit_offset = function()
    g.server:exec(function()
        local res = box.execute([[SELECT 1 LIMIT 1;]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT 1 LIMIT 1 OFFSET 1;]])
        t.assert_equals(res.rows, {})
        res = box.execute([[SELECT 1 LIMIT 0, 1;]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT 1 LIMIT (SELECT 1) OFFSET (SELECT 0);]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT (SELECT 1 LIMIT 1 OFFSET 0);]])
        t.assert_equals(res.rows, {{1}})
        res = box.execute([[SELECT (SELECT 1 LIMIT 1 OFFSET 1);]])
        t.assert_equals(res.rows, {{box.NULL}})

        local exp_err = "Failed to execute SQL statement: Only positive " ..
                        "integers are allowed in the LIMIT clause"
        local _, err = box.execute([[SELECT 1 LIMIT -1;]])
        t.assert_equals(err.message, exp_err)
    end)
end

--
-- LIMIT and OFFSET cannot refer to the result columns or use aggregate
-- functions, even in a subquery in the column list, where aggregate functions
-- are allowed.
--
g.test_limit_offset_names_and_aggregates = function()
    g.server:exec(function()
        local _, err = box.execute([[SELECT 1 AS a LIMIT a;]])
        t.assert_equals(err.message, "Can't resolve field 'a'")
        _, err = box.execute([[SELECT 1 AS a LIMIT 1 OFFSET a;]])
        t.assert_equals(err.message, "Can't resolve field 'a'")

        local exp_err = "misuse of aggregate function COUNT()"
        _, err = box.execute([[SELECT 1 LIMIT COUNT(*);]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT 1 LIMIT 1 OFFSET COUNT(*);]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT (SELECT 1 LIMIT COUNT(*));]])
        t.assert_equals(err.message, exp_err)
        _, err = box.execute([[SELECT ABS((SELECT 1 LIMIT COUNT(*)));]])
        t.assert_equals(err.message, exp_err)
    end)
end

--
-- LIMIT and OFFSET are resolved before the columns, and LIMIT before OFFSET,
-- whatever order they are written in.
--
g.test_limit_offset_error_order = function()
    g.server:exec(function()
        local _, err = box.execute([[SELECT a LIMIT b OFFSET c;]])
        t.assert_equals(err.message, "Can't resolve field 'b'")
        _, err = box.execute([[SELECT a LIMIT b, c;]])
        t.assert_equals(err.message, "Can't resolve field 'c'")
        _, err = box.execute([[SELECT * LIMIT b;]])
        t.assert_equals(err.message, "Failed to expand '*' in SELECT " ..
                                     "statement without FROM clause")
    end)
end

--
-- An ORDER BY term which is an alias of a result column or an integer refers
-- to the result column. The columns without aliases are named automatically
-- in the order the SELECTs are resolved, and can be referred to by these names.
--
g.test_order_by_result_columns = function()
    g.server:exec(function()
        local function check(sql, exp)
            local res, err = box.execute(sql)
            if type(exp) == 'string' then
                t.assert_equals(res, nil, sql)
                t.assert_equals(err.message, exp, sql)
            else
                t.assert_equals(err, nil, sql)
                t.assert_equals(res.rows, exp, sql)
            end
        end
        local function range_err(n)
            return 'Error at ORDER BY in place ' .. n .. ': term out ' ..
                   'of range - should be between 1 and 1'
        end
        local function name_err(name)
            return "Can't resolve field '" .. name .. "'"
        end

        check([[SELECT 2 AS a, 1 ORDER BY a, 2 DESC;]], {{2, 1}})
        check([[SELECT 1 AS A ORDER BY a;]], {{1}})
        check([[SELECT 1 AS a ORDER BY A;]], name_err('A'))
        check([[SELECT 1 AS a ORDER BY "A";]], name_err('A'))
        check([[SELECT 'a' AS a ORDER BY (a) COLLATE "unicode_ci";]], {{'a'}})
        check([[SELECT 'a' ORDER BY 1 COLLATE "unicode_ci";]], {{'a'}})
        check([[SELECT 1 ORDER BY 1 COLLATE "nosuch";]],
              "Collation 'nosuch' does not exist")

        check([[SELECT 1, 2 ORDER BY COLUMN_2;]], {{1, 2}})
        check([[SELECT 1, (SELECT 1 ORDER BY COLUMN_3);]], {{1, 1}})
        check([[SELECT 1, (SELECT 1 ORDER BY COLUMN_2);]],
              name_err('COLUMN_2'))
        check([[SELECT 1 LIMIT (SELECT 1 ORDER BY COLUMN_2);]], {{1}})
        check([[SELECT 1 AS x, (SELECT 1 ORDER BY COLUMN_1);]],
              name_err('COLUMN_1'))

        check([[SELECT 1 ORDER BY +1, (1);]], {{1}})
        check([[SELECT 1 ORDER BY 0;]], range_err(1))
        check([[SELECT 1 ORDER BY -1;]], range_err(1))
        check([[SELECT 1 ORDER BY 1, 2;]], range_err(2))
        check([[SELECT 1 ORDER BY 65536;]], range_err(1))
        check([[SELECT 1 ORDER BY -2147483648;]], range_err(1))
        check([[SELECT 1 ORDER BY 2147483648, -2147483649;]], {{1}})
        check([[SELECT 1 ORDER BY 1.0, CAST(2 AS INTEGER), 1 + 1;]], {{1}})

        check([[SELECT 1 ORDER BY 0, nosuch;]], range_err(1))
        check([[SELECT 1 ORDER BY 2, nosuch;]], name_err('nosuch'))
        check([[SELECT nosuch1 ORDER BY nosuch2;]], name_err('nosuch1'))
        check([[SELECT 1 ORDER BY nosuch1 LIMIT nosuch2;]],
              name_err('nosuch2'))

        local exp_err = 'Type mismatch: can not convert array to ' ..
                        'comparable type'
        check([[SELECT [1] ORDER BY 1;]], exp_err)
        check([[SELECT [1] AS a ORDER BY a;]], exp_err)
        check([[SELECT 1 ORDER BY [1];]], exp_err)
        check([[SELECT [1] AS a ORDER BY a + 0;]], {{{1}}})
    end)
end

--
-- The aliases of the result columns are visible in the ORDER BY expressions and
-- in their subqueries, but not in the ORDER BY of a subquery.
--
g.test_order_by_aliases = function()
    g.server:exec(function()
        local function check(sql, exp)
            local res, err = box.execute(sql)
            if type(exp) == 'string' then
                t.assert_equals(res, nil, sql)
                t.assert_equals(err.message, exp, sql)
            else
                t.assert_equals(err, nil, sql)
                t.assert_equals(res.rows, exp, sql)
            end
        end
        local function chain(n)
            return '1' .. string.rep(' + 0', n - 1)
        end

        check([[SELECT 2 AS a, 1 AS a ORDER BY a + 0;]], {{2, 1}})
        check([[SELECT 1 AS "A", 2 AS a ORDER BY a + 0;]], {{1, 2}})
        check([[SELECT 1 AS a ORDER BY (SELECT a + 1), 1 IN (SELECT a),
                EXISTS (SELECT a);]], {{1}})
        check([[SELECT 1 AS a ORDER BY (SELECT (SELECT a));]], {{1}})
        check([[SELECT SUM(1) AS s ORDER BY (SELECT (SELECT s));]], {{1}})
        check([[SELECT 1 ORDER BY COUNT(*);]], {{1}})
        check([[SELECT 1 AS a ORDER BY a + (SELECT 1 ORDER BY a);]],
              "Can't resolve field 'a'")
        check([[SELECT (SELECT 1 AS b ORDER BY a) AS a;]],
              "Can't resolve field 'a'")
        check([[SELECT 1 AS a ORDER BY t.a;]],
              "Field 'a' was not found in space 't' format")
        check([[SELECT (1, 2) AS a ORDER BY a + 0;]], 'row value misused')

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        check('SELECT ' .. chain(199) .. ' AS a ORDER BY a + 0;', {{1}})
        check('SELECT ' .. chain(200) .. ' AS a ORDER BY a + 0;', exp_err)
        check('SELECT ' .. chain(200) .. ' AS a ORDER BY a;', {{1}})
        check('SELECT ' .. chain(200) .. ' AS a ORDER BY a COLLATE ' ..
              '"binary";', exp_err)
    end)
end
