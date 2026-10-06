local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({alias = 'master'})
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

--
-- ghs-160: The built-in REPLACE function threw a segmentation fault if
-- the length of the first argument was less than the length of the second.
--
g.test_segfault_in_replace = function(cg)
    cg.server:exec(function()
        local res = box.execute([[SELECT REPLACE('', 'ab', 'xy');]])
        t.assert_equals(res.rows, {{''}})
        res = box.execute([[SELECT REPLACE('a', 'ab', 'xy');]])
        t.assert_equals(res.rows, {{'a'}})
    end)
end

--
-- ghs-161: The PRINTF built-in function could have an integer overflow.
--
g.test_integer_overflow_in_printf = function(cg)
    cg.server:exec(function()
        local exp = "Failed to execute SQL statement: string or blob too big"
        local sql = [[SELECT LENGTH(PRINTF('%2200000000s', 'a'));]]
        local _, err = box.execute(sql)
        t.assert_equals(err.message, exp)
    end)
end

--
-- gh-13353: make sure that the trimming side is not taken from the arguments
-- of TRIM().
--
g.test_trim_side = function(cg)
    cg.server:exec(function()
        local cases = {
            {[[TRIM('  s  ')]], 's'},
            {[[TRIM(LEADING FROM '  s  ')]], 's  '},
            {[[TRIM(TRAILING FROM '  s  ')]], '  s'},
            {[[TRIM(BOTH FROM '  s  ')]], 's'},
            {[[TRIM('x' FROM 'xsx')]], 's'},
            {[[TRIM(LEADING 'x' FROM 'xsx')]], 'sx'},
            {[[TRIM(TRAILING 'x' FROM 'xsx')]], 'xs'},
            {[[TRIM(BOTH 'x' FROM 'xsx')]], 's'},
            {[[TRIM(LEADING FROM X'000100')]], '\1\0'},
            {[[TRIM(TRAILING X'00' FROM X'000100')]], '\0\1'},
            {[["TRIM"('xsx', 'x')]], 's'},
        }
        for _, case in pairs(cases) do
            local res = box.execute('SELECT ' .. case[1] .. ';')
            t.assert_equals(res.rows, {{case[2]}}, case[1])
        end

        local exp = 'Failed to execute SQL statement: wrong arguments for ' ..
                    'function TRIM()'
        for _, expr in pairs({[[TRIM(1 FROM '  s  ')]],
                              [[TRIM(-1 FROM '  s  ')]],
                              [[TRIM(-1 FROM X'20')]],
                              [[TRIM(LEADING 1 FROM '  s  ')]],
                              [["TRIM"('  s  ', 1)]],
                              [["TRIM"('  s  ', -1)]],
                              [["TRIM"(X'20', -1)]]}) do
            local _, err = box.execute('SELECT ' .. expr .. ';')
            t.assert_equals(err.message, exp, expr)
        end
        local _, err = box.execute([[SELECT "TRIM"('xsx', 3, 'x');]])
        t.assert_equals(err.message, 'Wrong number of arguments is passed ' ..
                        'to TRIM(): expected from 1 to 2, got 3')
    end)
end

--
-- gh-13353: make sure that calls of TRIM() that trim different sides are not
-- considered equal.
--
g.test_trim_side_compare = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (i INT PRIMARY KEY, s STRING);]])
        box.execute([[INSERT INTO t VALUES (1, ' a '), (2, ' a'), (3, 'a ');]])
        local res = box.execute([[SELECT TRIM(LEADING FROM s),
                                         TRIM(TRAILING FROM s), COUNT(*)
                                  FROM SEQSCAN t
                                  GROUP BY TRIM(LEADING FROM s),
                                           TRIM(TRAILING FROM s)
                                  ORDER BY 1, 2;]])
        t.assert_equals(res.rows, {{'a', ' a', 1}, {'a ', ' a', 1},
                                   {'a ', 'a', 1}})
        res = box.execute([[SELECT TRIM(LEADING FROM s) FROM SEQSCAN t
                            ORDER BY TRIM(TRAILING FROM s), i;]])
        t.assert_equals(res.rows, {{'a '}, {'a'}, {'a '}})
        box.execute([[DROP TABLE t;]])
    end)
end
