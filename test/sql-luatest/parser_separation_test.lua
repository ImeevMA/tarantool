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
-- Make sure that the ALTER TABLE RENAME statement is processed only after
-- successful parsing.
--
g.test_table_rename_parsing = function()
    g.server:exec(function()
        local _, err = box.execute([[ALTER TABLE t RENAME TO t1 1;]])
        t.assert_equals(err.message, "Syntax error at line 1 near '1'")
    end)
end

--
-- Make sure that the ALTER TABLE DROP CONSTRAINT statement is processed only
-- after successful parsing.
--
g.test_drop_constraint_parsing = function()
    g.server:exec(function()
        local _, err = box.execute([[ALTER TABLE t DROP CONSTRAINT t1 1;]])
        t.assert_equals(err.message, "Syntax error at line 1 near '1'")
    end)
end

--
-- Make sure that the DROP INDEX statement is processed only after successful
-- parsing.
--
g.test_drop_index_parsing = function()
    g.server:exec(function()
        local _, err = box.execute([[DROP INDEX i1 on t1 1;]])
        t.assert_equals(err.message, "Syntax error at line 1 near '1'")
    end)
end

--
-- Make sure that the DROP VIEW statement is processed only after successful
-- parsing.
--
g.test_drop_view_parsing = function()
    g.server:exec(function()
        local _, err = box.execute([[DROP VIEW v 1;]])
        t.assert_equals(err.message, "Syntax error at line 1 near '1'")
    end)
end

--
-- Make sure that the DROP TABLE statement is processed only after successful
-- parsing.
--
g.test_drop_table_parsing = function()
    g.server:exec(function()
        local _, err = box.execute([[DROP TABLE t 1;]])
        t.assert_equals(err.message, "Syntax error at line 1 near '1'")
    end)
end

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

--
-- Make sure there is no segmentation fault or assertion in case CHECK is
-- declared before the first column.
--
g.test_index_check_parsing = function()
    g.server:exec(function()
        local sql = [[CREATE TABLE t(CONSTRAINT c1 CHECK(i > 10),
                                     i INT PRIMARY KEY);]]
        local res = box.execute(sql)
        t.assert_equals(res, {row_count = 1})
        local func_id = box.space._func.index.name:get{'check_T_C1'}.id
        t.assert_equals(box.space.T.constraint, {C1 = func_id})
        box.execute([[DROP TABLE t;]])
    end)
end

--
-- Make sure that the creation of the UNIQUE constraint is processed after the
-- creation of the columns and the processing of the PRIMARY KEY.
--
g.test_index_unique_parsing = function()
    g.server:exec(function()
        local sql = [[CREATE TABLE t(UNIQUE(a), i INT PRIMARY KEY, a INT);]]
        t.assert_equals(box.execute(sql), {row_count = 1})
        t.assert_equals(box.space.T.index[0].name, 'pk_unnamed_T_1')
        t.assert_equals(box.space.T.index[1].name, 'unique_unnamed_T_2')
        box.execute([[DROP TABLE t;]])
    end)
end
