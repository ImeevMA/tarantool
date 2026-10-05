local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
    cg.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY);]])
        box.execute([[INSERT INTO t VALUES (1);]])
    end)
end)

g.after_all(function(cg)
    cg.server:drop()
end)

--
-- Make sure an AND with an always FALSE operand is FALSE, and its other
-- operand is not resolved. An IN with an empty list is always FALSE too.
--
g.test_and_false_folding = function(cg)
    cg.server:exec(function()
        for _, where in pairs({
            "false AND nosuch",
            "nosuch AND false",
            "a IN () AND nosuch",
            "nosuch AND a IN ()",
            "(a IN ()) AND (SELECT nosuch FROM nosuch)",
            "(false AND nosuch) AND nosuch",
        }) do
            local sql = "SELECT * FROM t WHERE " .. where .. ";"
            local res, err = box.execute(sql)
            t.assert_equals(err, nil, sql)
            t.assert_equals(res.rows, {}, sql)
        end
        local _, err = box.execute([[SELECT * FROM t WHERE a IN () OR
                                     nosuch;]])
        t.assert_equals(err.message, "Can't resolve field 'nosuch'")
    end)
end
