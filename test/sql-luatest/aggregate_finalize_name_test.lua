local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'aggregate-finalize-name'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

-- Make sure the finalizer is found by the name of the called function, not
-- by the name as it is written in the query.
g.test_aggregate_finalize_unquoted_name = function()
    g.server:exec(function()
        box.schema.func.create('AGG', {
            language = 'Lua',
            returns = 'integer',
            aggregate = 'group',
            body = 'function(x, s) return (s or 0) + x end',
            exports = {'SQL'},
            param_list = {'integer', 'integer'},
        })
        box.schema.func.create('AGG_finalize', {
            language = 'Lua',
            returns = 'string',
            body = 'function(s) return "res: " .. tostring(s) end',
            exports = {'LUA', 'SQL'},
            param_list = {'integer'},
        })
        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        box.execute([[INSERT INTO t VALUES (1), (2);]])

        local metadata = {
            {name = 'COLUMN_1', type = 'string'},
            {name = 'COLUMN_2', type = 'string'},
        }
        local res, err = box.execute([[SELECT AGG(1), agg(1);]])
        t.assert_equals(err, nil)
        t.assert_equals(res.metadata, metadata)
        t.assert_equals(res.rows, {{'res: 1', 'res: 1'}})

        res, err = box.execute([[SELECT AGG(i), agg(i) FROM SEQSCAN t;]])
        t.assert_equals(err, nil)
        t.assert_equals(res.metadata, metadata)
        t.assert_equals(res.rows, {{'res: 3', 'res: 3'}})

        box.execute([[DROP TABLE t;]])
        box.schema.func.drop('AGG_finalize')
        box.schema.func.drop('AGG')
    end)
end
