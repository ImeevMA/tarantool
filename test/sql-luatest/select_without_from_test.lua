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

--
-- WHERE is resolved after the columns and before ORDER BY. The aliases of
-- the result columns and the names of the enclosing SELECTs are visible in it
-- and in its subqueries.
--
g.test_where = function()
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

        check([[SELECT 1 WHERE true;]], {{1}})
        check([[SELECT 1 WHERE false;]], {})
        check([[SELECT 1 WHERE NULL;]], {})
        check([[SELECT 1 WHERE false LIMIT 1;]], {})
        check([[SELECT 1 AS a WHERE a = 1;]], {{1}})
        check([[SELECT 2 AS a WHERE a = 1;]], {})
        check([[SELECT 1 AS "A" WHERE a = 1;]], {{1}})
        check([[SELECT 1 AS a, 2 AS a WHERE a = 2;]], {})
        check([[SELECT 1 AS a WHERE (SELECT a) = 1 AND
                EXISTS (SELECT 1 WHERE a = 1);]], {{1}})
        check([[SELECT 1 AS a WHERE (SELECT 2 AS a WHERE a = 2) = 2;]],
              {{1}})
        check([[SELECT (SELECT 5 AS b WHERE b < 1);]], {{box.NULL}})
        check([[SELECT 1 AS a ORDER BY (SELECT 2 WHERE a = 1);]], {{1}})
        check([[SELECT 1 LIMIT (SELECT 1 WHERE true);]], {{1}})
        check([[SELECT COUNT(*), 1 AS d WHERE d = 2;]], {{0, 1}})
        check([[SELECT 1 WHERE (SELECT COUNT(*)) > 0;]], {{1}})

        check([[SELECT 1 AS a WHERE "A" = 1;]], "Can't resolve field 'A'")
        check([[SELECT 1 WHERE t.a;]],
              "Field 'a' was not found in space 't' format")
        check([[SELECT (1, 2) AS a WHERE a = 1;]], 'row value misused')
        check([[SELECT 1 AS a WHERE (SELECT 1 WHERE a = 1 LIMIT a);]],
              "Can't resolve field 'a'")
        check([[SELECT nosuch1 WHERE nosuch2;]],
              "Can't resolve field 'nosuch1'")
        check([[SELECT 1 WHERE nosuch1 ORDER BY nosuch2;]],
              "Can't resolve field 'nosuch1'")
        check([[SELECT 1 WHERE nosuch1 LIMIT nosuch2;]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT 1 WHERE 5;]],
              'Type mismatch: can not convert integer(5) to boolean')

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        check('SELECT ' .. chain(199) .. ' AS a WHERE a = 1;', {{1}})
        check('SELECT ' .. chain(200) .. ' AS a WHERE a = 1;', exp_err)
    end)
end

--
-- Aggregate functions cannot be used in WHERE, either directly or through
-- an alias, even in an aggregate SELECT. An alias of an aggregate cannot be
-- used in an argument of an aggregate function.
--
g.test_where_aggregates = function()
    g.server:exec(function()
        local function check(sql, exp)
            local _, err = box.execute(sql)
            t.assert_equals(err.message, exp, sql)
        end
        local function misuse(name)
            return 'misuse of aggregate function ' .. name .. '()'
        end
        local function alias_misuse(name)
            return 'misuse of aliased aggregate ' .. name
        end

        check([[SELECT 1 WHERE COUNT(*) > 0;]], misuse('COUNT'))
        check([[SELECT COUNT(*) WHERE COUNT(*) > 0;]], misuse('COUNT'))
        check([[SELECT MAX(1) WHERE MIN(1) > 0;]], misuse('MIN'))
        check([[SELECT 1 WHERE (SELECT 1 WHERE COUNT(*));]], misuse('COUNT'))
        check([[SELECT COUNT(*) WHERE COUNT(*) > 0 ORDER BY nosuch;]],
              misuse('COUNT'))

        check([[SELECT COUNT(*) AS c WHERE c > 0;]], alias_misuse('c'))
        check([[SELECT 1 + COUNT(*) AS c WHERE c > 0;]], alias_misuse('c'))
        check([[SELECT COUNT(*) AS "C c" WHERE "C c" > 0;]],
              alias_misuse('C c'))
        check([[SELECT COUNT(*) AS c WHERE (SELECT c) > 0;]],
              alias_misuse('c'))
        check([[SELECT 1 AS c WHERE (SELECT COUNT(*) AS c WHERE c > 0) > 0;]],
              alias_misuse('c'))
        check([[SELECT COUNT(*) AS c ORDER BY SUM(c);]], alias_misuse('c'))
        check([[SELECT COUNT(*) AS c ORDER BY SUM((SELECT c));]],
              alias_misuse('c'))
    end)
end

--
-- A GROUP BY term which is an integer refers to the result column, and any
-- other term is an expression, in which the aliases of the result columns are
-- visible, but not the names of the enclosing SELECTs. GROUP BY is resolved
-- after ORDER BY.
--
g.test_group_by = function()
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
            return 'Error at GROUP BY in place ' .. n .. ': term out ' ..
                   'of range - should be between 1 and 1'
        end
        local function chain(n)
            return '1' .. string.rep(' + 0', n - 1)
        end

        check([[SELECT 1 GROUP BY 1;]], {{1}})
        check([[SELECT 1 AS a GROUP BY a, a + 1, (1), +1, 1.5, NULL;]], {{1}})
        check([[SELECT 1 AS a GROUP BY 2147483648;]], {{1}})
        check([[SELECT 'a' AS a GROUP BY a COLLATE "unicode_ci";]], {{'a'}})
        check([[SELECT 1 AS a GROUP BY (SELECT a);]], {{1}})
        check([[SELECT COUNT(*), 5 AS a GROUP BY a;]], {{1, 5}})
        check([[SELECT 2 AS a, 1 GROUP BY a, 2 ORDER BY 1;]], {{2, 1}})
        check([[SELECT 1 GROUP BY (SELECT COUNT(*));]], {{1}})

        check([[SELECT 1 GROUP BY 0;]], range_err(1))
        check([[SELECT 1 GROUP BY -1;]], range_err(1))
        check([[SELECT 1 GROUP BY 70000;]], range_err(1))
        check([[SELECT 1 GROUP BY 1, 2;]], range_err(2))
        check([[SELECT 1 GROUP BY 2, nosuch;]], "Can't resolve field 'nosuch'")
        check([[SELECT 1 GROUP BY 1 COLLATE "binary";]],
              "COLLATE clause can't be used with non-string arguments")
        check([[SELECT 1 GROUP BY t.a;]],
              "Field 'a' was not found in space 't' format")
        check([[SELECT (1, 2) AS a GROUP BY a;]], 'row value misused')
        check([[SELECT [1] AS a GROUP BY a;]],
              'Type mismatch: can not convert array to comparable type')
        check([[SELECT 1 AS a, (SELECT 1 GROUP BY a);]],
              "Can't resolve field 'a'")
        check([[SELECT 1 AS a ORDER BY (SELECT 1 GROUP BY a);]],
              "Can't resolve field 'a'")
        check([[SELECT 1 WHERE (SELECT 1 GROUP BY nosuch1) ORDER BY nosuch2;]],
              "Can't resolve field 'nosuch1'")
        check([[SELECT 1 GROUP BY nosuch1 ORDER BY nosuch2;]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT 1 WHERE nosuch1 GROUP BY nosuch2;]],
              "Can't resolve field 'nosuch1'")

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        check('SELECT ' .. chain(200) .. ' AS a GROUP BY a;', {{1}})
        check('SELECT ' .. chain(199) .. ' AS a GROUP BY a + 0;', {{1}})
        check('SELECT ' .. chain(200) .. ' AS a GROUP BY a + 0;', exp_err)
        check('SELECT (SELECT 1 GROUP BY ' .. chain(199) .. ');', {{1}})
        check('SELECT (SELECT 1 GROUP BY ' .. chain(200) .. ');', exp_err)
    end)
end

--
-- Aggregate functions cannot be used in GROUP BY, either directly or through
-- the result columns.
--
g.test_group_by_aggregates = function()
    g.server:exec(function()
        local exp_err = 'aggregate functions are not allowed in the ' ..
                        'GROUP BY clause'
        local sqls = {
            [[SELECT 1 GROUP BY COUNT(*);]],
            [[SELECT 1 GROUP BY ABS(COUNT(*));]],
            [[SELECT COUNT(*) GROUP BY 1;]],
            [[SELECT COUNT(*) + 1 GROUP BY COUNT(*) + 1;]],
            [[SELECT COUNT(*) AS c GROUP BY c;]],
            [[SELECT COUNT(*) AS c GROUP BY c + 1;]],
            [[SELECT COUNT(*) AS c GROUP BY (SELECT c);]],
            [[SELECT COUNT(*) AS c GROUP BY 1 HAVING c = 1;]],
        }
        for _, sql in ipairs(sqls) do
            local _, err = box.execute(sql)
            t.assert_equals(err.message, exp_err, sql)
        end
        local _, err = box.execute([[SELECT 1 GROUP BY SUM(COUNT(*));]])
        t.assert_equals(err.message, 'misuse of aggregate function COUNT()')
    end)
end

--
-- HAVING is resolved after the columns and before WHERE. The aliases of the
-- result columns and the names of the enclosing SELECTs are visible in it.
-- Without GROUP BY, the SELECT must be an aggregate one, HAVING can refer to
-- the result columns only in arguments of aggregate functions, and LIMIT is
-- replaced with 1.
--
g.test_having = function()
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

        check([[SELECT 1 AS a GROUP BY 1 HAVING a = 1;]], {{1}})
        check([[SELECT 1 AS a GROUP BY 1 HAVING a = 2;]], {})
        check([[SELECT 1 GROUP BY 1 HAVING COUNT(*) = 1;]], {{1}})
        check([[SELECT 1 GROUP BY 1 HAVING SUM(1) > 0 LIMIT 5;]], {{1}})
        check([[SELECT 1 AS a GROUP BY 1 HAVING (SELECT a) = 1;]], {{1}})
        check([[SELECT 1 AS a GROUP BY a HAVING a = 1 AND COUNT(*) = 1;]],
              {{1}})
        check([[SELECT COUNT(*) AS c GROUP BY 'x' HAVING (SELECT c) > 0;]],
              {{1}})

        check([[SELECT COUNT(*) HAVING true;]], {{1}})
        check([[SELECT COUNT(*) HAVING false;]], {})
        check([[SELECT 1 HAVING COUNT(*) > 0;]], {{1}})
        check([[SELECT MIN(5) HAVING MAX(1) > 0;]], {{5}})
        check([[SELECT 1 AS a HAVING SUM(a) > 0;]], {{1}})
        check([[SELECT 1 AS a HAVING COUNT(*) > 0 AND (SELECT a) = 1;]], {{1}})
        check([[SELECT COUNT(*) AS c HAVING (SELECT c) > 0;]], {{1}})
        check([[SELECT COUNT(*) HAVING true LIMIT 0;]], {{1}})
        check([[SELECT COUNT(*) HAVING true LIMIT 'a';]], {{1}})
        check([[SELECT COUNT(*) HAVING true LIMIT 5 OFFSET 1;]], {})
        check([[SELECT (SELECT COUNT(*) HAVING true LIMIT 0);]], {{1}})

        local exp_err = 'Failed to execute SQL statement: HAVING argument ' ..
                        'must appear in the GROUP BY clause or be used in ' ..
                        'an aggregate function'
        check([[SELECT 1 HAVING true;]], exp_err)
        check([[SELECT 1 HAVING (SELECT COUNT(*));]], exp_err)
        check([[SELECT COUNT(*) AS c HAVING c > 0;]], exp_err)
        check([[SELECT 1 AS a HAVING a > 0 AND COUNT(*) > 0;]], exp_err)
        check([[SELECT 1 AS a HAVING (SELECT COUNT(*) HAVING a = 1);]],
              exp_err)
        check([[SELECT 1 AS a GROUP BY 1 HAVING
                (SELECT COUNT(*) HAVING a = 1);]], exp_err)

        check([[SELECT COUNT(*) AS c HAVING SUM(c) > 0;]],
              'misuse of aliased aggregate c')
        check([[SELECT 1 GROUP BY 1 HAVING SUM(COUNT(*));]],
              'misuse of aggregate function COUNT()')
        check([[SELECT COUNT(*) HAVING 5;]],
              'Type mismatch: can not convert integer(5) to boolean')
        check([[SELECT COUNT(*) HAVING true LIMIT nosuch;]],
              "Can't resolve field 'nosuch'")
        check([[SELECT 1 WHERE nosuch1 HAVING nosuch2;]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT 1 GROUP BY nosuch1 HAVING nosuch2;]],
              "Can't resolve field 'nosuch2'")

        exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                  'the limit (200)'
        check('SELECT COUNT(*) HAVING ' .. chain(200) .. ' > 0;', exp_err)
        check('SELECT (SELECT 1 GROUP BY 1 HAVING ' .. chain(198) ..
              ' > 0);', {{1}})
        check('SELECT (SELECT 1 GROUP BY 1 HAVING ' .. chain(199) ..
              ' > 0);', exp_err)
    end)
end

--
-- The parts of a compound SELECT are resolved from right to left, and so are
-- their columns named automatically. All the parts must have the same number
-- of columns, and only the rightmost one can have ORDER BY and LIMIT.
--
g.test_compound = function()
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

        check([[SELECT 1 UNION SELECT 2;]], {{1}, {2}})
        check([[SELECT 1 UNION SELECT 1;]], {{1}})
        check([[SELECT 1 UNION ALL SELECT 1;]], {{1}, {1}})
        check([[SELECT 1 EXCEPT SELECT 1;]], {})
        check([[SELECT 1 INTERSECT SELECT 2;]], {})
        check([[SELECT 2 AS a INTERSECT SELECT 2 UNION ALL SELECT 3;]],
              {{2}, {3}})
        check([[SELECT 1 AS a WHERE a = 1 UNION ALL
                SELECT 2 AS b WHERE b = 2;]], {{1}, {2}})
        check([[SELECT 1 UNION ALL SELECT 2 LIMIT 1 OFFSET 1;]], {{2}})
        check([[SELECT 1 UNION ALL SELECT COUNT(*) HAVING true;]], {{1}})
        check([[SELECT (SELECT 1 UNION SELECT 2 EXCEPT SELECT 1);]], {{2}})
        check([[SELECT 1 AS x WHERE x IN (SELECT 1 UNION SELECT x);]], {{1}})
        check([[SELECT EXISTS (SELECT 1 EXCEPT SELECT 1);]], {{false}})
        check([[SELECT 1, 2 UNION ALL SELECT 3, 4 ORDER BY COLUMN_3;]],
              {{1, 2}, {3, 4}})
        check([[SELECT 1 UNION ALL SELECT 2 ORDER BY COLUMN_1;]],
              "Can't resolve field 'COLUMN_1'")

        check([[SELECT 1 AS a UNION ALL SELECT 2 WHERE a = 2;]],
              "Can't resolve field 'a'")
        check([[SELECT 1 AS a UNION ALL SELECT (SELECT a);]],
              "Can't resolve field 'a'")
        check([[SELECT nosuch1 UNION ALL SELECT nosuch2;]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT nosuch1 UNION ALL SELECT 1 LIMIT nosuch2;]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT nosuch UNION ALL SELECT *;]], "Failed to expand " ..
              "'*' in SELECT statement without FROM clause")

        local function count_err(op)
            return 'SELECTs to the left and right of ' .. op .. ' do not ' ..
                   'have the same number of result columns'
        end
        check([[SELECT 1, 2 UNION SELECT 3;]], count_err('UNION'))
        check([[SELECT 1 UNION ALL SELECT 2 UNION SELECT 3, 4;]],
              count_err('UNION'))
        check([[SELECT 1, 2 UNION ALL SELECT 3 EXCEPT SELECT 4;]],
              count_err('UNION ALL'))
        check([[SELECT (1, 2) IN (SELECT 1, 2 UNION SELECT 3);]],
              count_err('UNION'))

        local function place_err(clause, op)
            return clause .. ' clause should come after ' .. op ..
                   ' not before'
        end
        check([[SELECT 2 ORDER BY 1 UNION ALL SELECT 1;]],
              place_err('ORDER BY', 'UNION ALL'))
        check([[SELECT 2 LIMIT 1 UNION SELECT 1;]],
              place_err('LIMIT', 'UNION'))
        check([[SELECT COUNT(*) HAVING true UNION ALL SELECT 1;]],
              place_err('LIMIT', 'UNION ALL'))
        check([[SELECT 3 LIMIT 1 UNION ALL SELECT 2 ORDER BY 1
                UNION SELECT 1;]], place_err('ORDER BY', 'UNION'))
        check([[SELECT 3 ORDER BY nosuch UNION ALL SELECT 1;]],
              "Can't resolve field 'nosuch'")

        local sql = 'SELECT 1' .. string.rep(' UNION ALL SELECT 1', 30) .. ';'
        check(sql, 'The number of UNION or EXCEPT or INTERSECT operations ' ..
              '31 exceeds the limit (30)')

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        check('SELECT (SELECT ' .. chain(199) .. ' UNION ALL SELECT 1 ' ..
              'ORDER BY 1 LIMIT 1);', {{1}})
        check('SELECT (SELECT ' .. chain(200) .. ' UNION ALL SELECT 1 ' ..
              'ORDER BY 1 LIMIT 1);', exp_err)
    end)
end

--
-- An ORDER BY term of a compound SELECT is an integer, an alias of a result
-- column of the leftmost part or an expression equal to a result column, and
-- refers to that column.
--
g.test_compound_order_by = function()
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
        local function match_err(n)
            return 'Error at ORDER BY in place ' .. n .. ': term does not ' ..
                   'match any column in the result set'
        end

        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY 1;]], {{1}, {2}})
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY +1, (1);]], {{1}, {2}})
        check([[SELECT 1 UNION ALL SELECT 2 ORDER BY 1 DESC LIMIT 1;]], {{2}})
        check([[SELECT 2 AS a UNION ALL SELECT 1 ORDER BY a;]], {{1}, {2}})
        check([[SELECT 1 AS "A" UNION ALL SELECT 2 ORDER BY a DESC;]],
              {{2}, {1}})
        check([[SELECT 2 AS a UNION ALL SELECT 1 ORDER BY (a);]], {{1}, {2}})
        check([[SELECT 1 + 1 AS a UNION ALL SELECT 1 ORDER BY 1 + 1;]],
              {{1}, {2}})
        check([[SELECT 2 AS a, 3 UNION ALL SELECT 1, 1 + 1 ORDER BY 1 + 1;]],
              {{1, 2}, {2, 3}})
        check([[SELECT COUNT(*) UNION ALL SELECT 5 ORDER BY COUNT(*);]],
              {{1}, {5}})
        check([[SELECT 'b' AS s UNION ALL SELECT 'a' ORDER BY s
                COLLATE "unicode_ci" DESC;]], {{'b'}, {'a'}})
        check([[SELECT 1 UNION ALL SELECT 2 ORDER BY 1 COLLATE "unicode";]],
              {{1}, {2}})
        check([[SELECT 'b' AS s UNION SELECT 'A' ORDER BY s;]],
              {{'A'}, {'b'}})
        check([[SELECT 'b' UNION SELECT 'a' ORDER BY 1 COLLATE "unicode_ci";]],
              {{'a'}, {'b'}})
        check([[SELECT (SELECT 1 AS a UNION ALL SELECT 2 ORDER BY a DESC
                LIMIT 1);]], {{2}})
        check([[SELECT 1 AS a ORDER BY (SELECT 5 UNION ALL SELECT a
                ORDER BY 1 LIMIT 1);]], {{1}})

        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY 0;]], range_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY -1;]], range_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY 5, nosuch;]],
              range_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY nosuch, 5;]],
              range_err(2))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY nosuch;]], match_err(1))
        check([[SELECT 2 AS a UNION ALL SELECT 1 ORDER BY a + 1;]],
              match_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY 1.0;]], match_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY 2147483648;]],
              match_err(1))
        check([[SELECT 2 UNION ALL SELECT 1 ORDER BY COUNT(*);]],
              match_err(1))
        check([[SELECT 1 AS x UNION ALL SELECT 2 ORDER BY x, 1 + nosuch;]],
              match_err(2))
        check([[SELECT 1 AS a UNION ALL SELECT 2 ORDER BY (SELECT a);]],
              match_err(1))
        check([[SELECT 1 AS a UNION ALL SELECT 2 ORDER BY "A";]],
              match_err(1))
        check([[SELECT 2 AS a UNION ALL SELECT 1 AS b ORDER BY b;]],
              "Can't resolve field 'b'")
        check([[SELECT 1 UNION ALL SELECT 2 ORDER BY 1 COLLATE "nosuch";]],
              "Collation 'nosuch' does not exist")
        check([[SELECT [1] UNION ALL SELECT [2] ORDER BY 1;]],
              'Type mismatch: can not convert array to comparable type')
    end)
end

--
-- VALUES with several rows is a compound SELECT of its rows. The number of its
-- rows is not limited.
--
g.test_values = function()
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
        local function rows(n)
            local list = {}
            for i = 1, n do
                list[i] = '(' .. i .. ')'
            end
            return 'VALUES ' .. table.concat(list, ', ')
        end

        check([[VALUES (1, 'a'), (2, 'b');]], {{1, 'a'}, {2, 'b'}})
        check([[VALUES (1), (COUNT(*));]], {{1}, {1}})
        check([[VALUES ((SELECT 1)), (2);]], {{1}, {2}})
        check([[VALUES (1), (2) UNION SELECT 3;]], {{1}, {2}, {3}})
        check([[SELECT 3 UNION VALUES (1);]], {{1}, {3}})
        check([[VALUES (1) INTERSECT VALUES (1);]], {{1}})
        check([[VALUES (1), (2) UNION ALL SELECT 3 ORDER BY 1 DESC
                LIMIT 2;]], {{3}, {2}})
        check([[VALUES (1), (2) UNION ALL SELECT 3 ORDER BY COLUMN_3;]],
              {{1}, {2}, {3}})
        check([[VALUES (1), (2) UNION ALL SELECT 3 ORDER BY COLUMN_2;]],
              "Can't resolve field 'COLUMN_2'")
        check([[SELECT 1 IN (VALUES (1), (2));]], {{true}})
        check([[SELECT (1, 2) IN (VALUES (1, 2), (3, 4));]], {{true}})
        check([[SELECT EXISTS (VALUES (1), (2));]], {{true}})
        check([[SELECT 1 AS a WHERE a IN (VALUES (a), (2));]], {{1}})

        local res, err = box.execute(rows(40) .. ';')
        t.assert_equals(err, nil)
        t.assert_equals(#res.rows, 40)
        check(rows(30) .. ' UNION ALL SELECT 31;', 'The number of UNION ' ..
              'or EXCEPT or INTERSECT operations 31 exceeds the limit (30)')

        -- The number of rows of VALUES is not limited.
        res, err = box.execute(rows(100000) .. ';')
        t.assert_equals(err, nil)
        t.assert_equals(#res.rows, 100000)
        t.assert_equals(res.rows[100000], {100000})

        local exp_err = 'all VALUES must have the same number of terms'
        check([[VALUES (1), (2, 3);]], exp_err)
        check([[VALUES (1), (2), (3, 4);]], exp_err)
        check([[SELECT 3, 4 UNION VALUES (1);]], exp_err)
        check([[VALUES (1, 2) UNION SELECT 3;]], 'SELECTs to the left and ' ..
              'right of UNION do not have the same number of result columns')
        check([[VALUES (nosuch1), (nosuch2);]],
              "Can't resolve field 'nosuch2'")
        check([[SELECT 1 AS a UNION ALL VALUES (a);]],
              "Can't resolve field 'a'")
        check([[VALUES (1), ((1, 2));]], 'row value misused')
    end)
end
