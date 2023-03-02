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
-- Now VDBE for FOREIGN KEY creation is built after column creation, NULL and
-- NOT NULL constraints, DEFAULT clause, COLLATE clause, PRIMARY KEY constraint,
-- UNIQUE constraints, CHECK constraints. However, since we no longer have
-- uniqueness for the constraint name, we can only check that the FOREIGN KEY is
-- created after the columns are created, no matter where in the SQL query the
-- FOREIGN KEY creation is.
--
-- Also, make sure there is no segmentation fault or assertion in case
-- FOREIGN KEY is declared before the first column.
--
g.test_foreign_key_parsing = function()
    g.server:exec(function()
        local sql = [[CREATE TABLE t(CONSTRAINT f1 FOREIGN KEY(a) REFERENCES t,
                                     i INT PRIMARY KEY, a INT);]]
        local res = box.execute(sql)
        t.assert_equals(res, {row_count = 1})
        local fk_def = {F1 = {field = {[2] = 1}, space = box.space.T.id}}
        t.assert_equals(box.space.T.foreign_key, fk_def)
        box.execute([[DROP TABLE t;]])
    end)
end
