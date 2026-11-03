local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

--
-- Make sure that COLLATE contributes to the expression tree height.
--
g.test_collate_counts_towards_height = function()
    g.server:exec(function()
        local expr = "'a'" .. string.rep(" || 'a'", 198)

        local _, err = box.execute('SELECT (' .. expr .. " || 'a');")
        t.assert_equals(err, nil)

        _, err = box.execute('SELECT (' .. expr .. ') COLLATE binary;')
        t.assert_equals(err, nil)

        local exp_err = 'Number of nodes in expression tree 201 exceeds ' ..
                        'the limit (200)'
        _, err = box.execute('SELECT (' .. expr .. " || 'a') COLLATE binary;")
        t.assert_equals(err.message, exp_err)

        expr = "'a'" .. string.rep(' COLLATE binary', 199)
        _, err = box.execute('SELECT ' .. expr .. ';')
        t.assert_equals(err, nil)

        _, err = box.execute('SELECT ' .. expr .. ' COLLATE binary;')
        t.assert_equals(err.message, exp_err)
    end)
end

--
-- Make sure that a long chain of COLLATE clauses is rejected instead of
-- overflowing the stack.
--
g.test_very_deep_collate = function()
    g.server:exec(function()
        local expr = "'a'" .. string.rep(' COLLATE binary', 100000)
        local _, err = box.execute('SELECT ' .. expr .. ';')
        t.assert_equals(err.message, 'Number of nodes in expression tree ' ..
                        '201 exceeds the limit (200)')
    end)
end

--
-- Make sure that COLLATE contributes to the total height of the
-- expressions of nested subqueries.
--
g.test_collate_in_nested_subqueries = function()
    g.server:exec(function()
        local expr = "('a'" .. string.rep(" || 'a'", 148) .. ') COLLATE binary'
        local _, err = box.execute('SELECT ' .. expr .. ' || (SELECT a ' ..
                                   'FROM (SELECT ' .. expr .. ' AS a));')
        t.assert_equals(err.message, 'Number of nodes in expression tree ' ..
                        '301 exceeds the limit (200)')
    end)
end
