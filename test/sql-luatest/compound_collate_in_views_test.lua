local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'compound-collate-in-views'})
    g.server:start()
    g.server:exec(function()
        box.execute([[SET SESSION "sql_seq_scan" = true;]])
        box.execute([[CREATE TABLE s (a INT PRIMARY KEY, x STRING);]])
        box.execute([[INSERT INTO s VALUES (1, 'q'), (2, 'Q'), (3, 'b');]])
    end)
end)

g.after_all(function()
    g.server:stop()
end)

--
-- A compound SELECT of a view or of a table of a WITH clause with COLLATE in
-- ORDER BY is executed as a SELECT from a subquery, as any other compound
-- SELECT, so the collation of ORDER BY is not used to compare the rows.
--
g.test_compound_collate_in_views = function()
    g.server:exec(function()
        local function check(sql, exp)
            local res, err = box.execute(sql)
            t.assert_equals(err, nil, sql)
            t.assert_equals(res.rows, exp, sql)
        end
        local coll = ' ORDER BY 1 COLLATE "unicode_ci"'
        local selects = {
            {"SELECT x FROM s INTERSECT SELECT 'Q'" .. coll, {{'Q'}}},
            {"SELECT x FROM s EXCEPT SELECT 'Q'" .. coll, {{'b'}, {'q'}}},
            {"SELECT x FROM s UNION SELECT 'B'" .. coll,
             {{'B'}, {'b'}, {'Q'}, {'q'}}},
        }
        for i, s in ipairs(selects) do
            check(s[1], s[2])
            box.execute('CREATE VIEW v' .. i .. ' AS ' .. s[1] .. ';')
            check('SELECT * FROM v' .. i .. ';', s[2])
            check('WITH c AS (' .. s[1] .. ') SELECT * FROM c;', s[2])
            box.execute('DROP VIEW v' .. i .. ';')
        end
    end)
end
