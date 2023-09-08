local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'gh-5310'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

-- Make sure that tuples with undescribed fields do not cause an error.
g.test_cursor_invalidation_1 = function()
    g.server:exec(function()
        local s = box.schema.space.create('t', {format = {'a'}})
        s:create_index('ii')
        s:insert({1,2,3,4,5})
        s:insert({2})
        t.assert_equals(box.execute([[SELECT * FROM t;]]).rows, {{1}, {2}})
        s:drop()
    end)
end

--
-- Make sure that tuples with fields described in tuple format but not described
-- in space format do not cause an error.
--
g.test_cursor_invalidation_2 = function()
    g.server:exec(function()
        local s = box.schema.space.create('t', {format = {{'a', 'integer'}}})
        s:create_index('ii', {parts = {{1, 'integer'}, {2, 'integer'},
                                       {3, 'integer'}, {4, 'integer'}}})
        s:insert({1,2,3,4})
        t.assert_equals(box.execute([[SELECT * FROM t;]]).rows, {{1}})
        s:drop()
    end)
end
