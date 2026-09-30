local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
    g.server:exec(function()
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, b INT);]])
        box.execute([[INSERT INTO t VALUES (1, 1), (2, 2);]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

-- Make sure HAVING without GROUP BY is resolved when the result set
-- contains a non-aggregate expression without column references.
g.test_having_non_aggregate_result = function()
    g.server:exec(function()
        local sql = [[SELECT (SELECT 1), COUNT(*) FROM SEQSCAN t
                      HAVING COUNT(*) > 0;]]
        local res, err = box.execute(sql)
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {{1, 2}})

        sql = [[SELECT (SELECT 1), COUNT(*) FROM SEQSCAN t
                HAVING SUM(b) > 3;]]
        res, err = box.execute(sql)
        t.assert_equals(err, nil)
        t.assert_equals(res.rows, {})

        sql = [[SELECT (SELECT 1), COUNT(*) FROM SEQSCAN t
                HAVING COUNT(c) > 0;]]
        res, err = box.execute(sql)
        t.assert_equals(res, nil)
        t.assert_equals(err.message, "Can't resolve field 'c'")

        sql = [[SELECT a FROM SEQSCAN t HAVING COUNT(c) > 0;]]
        res, err = box.execute(sql)
        t.assert_equals(res, nil)
        t.assert_equals(err.message, "Can't resolve field 'c'")
    end)
end
