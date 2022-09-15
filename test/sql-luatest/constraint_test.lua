local server = require('test.luatest_helpers.server')
local t = require('luatest')
local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'constraints'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

-- Make sure ALTER TABLE ADD COLUMN does not drop field constraints.
g.test_constraints_1 = function()
    g.server:exec(function()
        local t = require('luatest')
        local fmt = {{'a', 'integer'}, {'b', 'integer'}}

        local body = "function(x) return true end"
        box.schema.func.create('ck1', {is_deterministic = true, body = body})
        local func_id = box.func.ck1.id
        fmt[1].constraint = {ck = 'ck1'}

        local s0 = box.schema.space.create('a', {format = fmt})
        local fk = {one = {field = 'a'}, two = {space = s0.id, field = 'b'}}
        fmt[2].foreign_key = fk

        local s = box.schema.space.create('b', {format = fmt})
        t.assert_equals(s:format()[1].constraint, {ck = func_id})
        t.assert_equals(s:format()[2].foreign_key, fk)
        box.execute([[ALTER TABLE "b" ADD COLUMN c INT;]])
        t.assert_equals(s:format()[1].constraint, {ck = func_id})
        t.assert_equals(s:format()[2].foreign_key, fk)
        box.space.b:drop()
        box.space.a:drop()
        box.schema.func.drop('ck1')
    end)
end

-- Make sure ALTER TABLE DROP CONSTRAINT drops field and tuple constraints.
g.test_constraints_2 = function()
    g.server:exec(function()
        local t = require('luatest')

        local body = "function(x) return true end"
        box.schema.func.create('ck1', {is_deterministic = true, body = body})
        local func_id = box.space._func.index[2]:get{'ck1'}.id

        local fk0 = {one = {field = {a = 'a'}}, two = {field = {b = 'b'}}}
        local ck0 = {three = 'ck1', four = 'ck1'}
        local fk1 = {five = {field = 'a'}, six = {field = 'b'}}
        local ck1 = {seven = 'ck1', eight = 'ck1'}

        local fmt = {{'a', 'integer'}, {'b', 'integer'}}
        fmt[1].constraint = ck1
        fmt[2].foreign_key = fk1

        local def = {format = fmt, foreign_key = fk0, constraint = ck0}
        local s = box.schema.space.create('a', def)
        ck0.three = func_id
        ck0.four = func_id
        ck1.seven = func_id
        ck1.eight = func_id
        t.assert_equals(s.foreign_key, fk0)
        t.assert_equals(s.constraint, ck0)
        t.assert_equals(s:format()[1].constraint, ck1)
        t.assert_equals(s:format()[2].foreign_key, fk1)

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "one";]])
        t.assert_equals(s.foreign_key, {two = {field = {b = 'b'}}})
        t.assert_equals(s.constraint, ck0)
        t.assert_equals(s:format()[1].constraint, ck1)
        t.assert_equals(s:format()[2].foreign_key, fk1)

        local _, err = box.execute([[ALTER TABLE "a" DROP CONSTRAINT "one";]])
        local res = [[Constraint 'one' does not exist in space 'a']]
        t.assert_equals(err.message, res)

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "four";]])
        t.assert_equals(s.foreign_key, {two = {field = {b = 'b'}}})
        t.assert_equals(s.constraint, {three = func_id})
        t.assert_equals(s:format()[1].constraint, ck1)
        t.assert_equals(s:format()[2].foreign_key, fk1)

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "seven";]])
        t.assert_equals(s.foreign_key, {two = {field = {b = 'b'}}})
        t.assert_equals(s.constraint, {three = func_id})
        t.assert_equals(s:format()[1].constraint, {eight = func_id})
        t.assert_equals(s:format()[2].foreign_key, fk1)

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "two";]])
        t.assert_equals(s.foreign_key, nil)
        t.assert_equals(s.constraint, {three = func_id})
        t.assert_equals(s:format()[1].constraint, {eight = func_id})
        t.assert_equals(s:format()[2].foreign_key, fk1)

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "five";]])
        t.assert_equals(s.foreign_key, nil)
        t.assert_equals(s.constraint, {three = func_id})
        t.assert_equals(s:format()[1].constraint, {eight = func_id})
        t.assert_equals(s:format()[2].foreign_key, {six = {field = 'b'}})

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "eight";]])
        t.assert_equals(s.foreign_key, nil)
        t.assert_equals(s.constraint, {three = func_id})
        t.assert_equals(s:format()[1].constraint, nil)
        t.assert_equals(s:format()[2].foreign_key, {six = {field = 'b'}})

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "three";]])
        t.assert_equals(s.foreign_key, nil)
        t.assert_equals(s.constraint, nil)
        t.assert_equals(s:format()[1].constraint, nil)
        t.assert_equals(s:format()[2].foreign_key, {six = {field = 'b'}})

        box.execute([[ALTER TABLE "a" DROP CONSTRAINT "six";]])
        t.assert_equals(s.foreign_key, nil)
        t.assert_equals(s.constraint, nil)
        t.assert_equals(s:format()[1].constraint, nil)
        t.assert_equals(s:format()[2].foreign_key, nil)

        _, err = box.execute([[ALTER TABLE "a" DROP CONSTRAINT "eight";]])
        res = [[Constraint 'eight' does not exist in space 'a']]
        t.assert_equals(err.message, res)

        box.space.a:drop()
        box.schema.func.drop('ck1')
    end)
end

-- Make sure field foreign key created properly.
g.test_constraints_3 = function()
    g.server:exec(function()
        local t = require('luatest')

        local sql = "CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t(i));"
        box.execute(sql)
        local res = {fk_unnamed_T_1 = {field = 1, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t(a));]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY REFERENCES t(a), a INT);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[1].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, ]]..
              [[a INT CONSTRAINT one REFERENCES t);]]
        box.execute(sql)
        res = {ONE = {field = 1, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t0 (i INT PRIMARY KEY, a INT);]])
        local space_id = box.space.T0.id

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t0(i));]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T0.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t0);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T0.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t0(a));]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T0.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY REFERENCES t0(a), a INT);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T0.id}}
        t.assert_equals(box.space.T:format()[1].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, ]]..
              [[a INT CONSTRAINT one REFERENCES t0);]]
        box.execute(sql)
        res = {ONE = {field = 1, space = box.space.T0.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t(i);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T.id}}
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t;]]
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t(a);]]
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT CONSTRAINT one REFERENCES t;]]
        res = {ONE = {field = 1, space = box.space.T.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t0(i);]]
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T0.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t0;]]
        res = {fk_unnamed_T_1 = {field = 1, space = box.space.T0.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT REFERENCES t0(a);]]
        res = {fk_unnamed_T_1 = {field = 2, space = box.space.T0.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT PRIMARY KEY);]])
        sql = [[ALTER TABLE t ADD COLUMN a INT CONSTRAINT one REFERENCES t0;]]
        res = {ONE = {field = 1, space = box.space.T0.id}}
        box.execute(sql)
        t.assert_equals(box.space.T:format()[2].foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.schema.space.create('T1')
        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t1);]]
        local _, err = box.execute(sql)
        local res = [[Failed to create foreign key constraint ]]..
            [['fk_unnamed_T_1': referenced space doesn't feature PRIMARY KEY]]
        t.assert_equals(err.message, res)
        box.space.T1:drop()

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT REFERENCES t(i, a));]]
        local _, err = box.execute(sql)
        local res = [[Failed to create foreign key constraint ]]..
            [['fk_unnamed_T_1': number of columns in foreign key does not ]]..
            [[match the number of columns in the primary index of ]]..
            [[referenced table]]
        t.assert_equals(err.message, res)

        box.execute([[CREATE VIEW v AS SELECT * FROM t0;]])
        sql = [[CREATE TABLE t (a INT REFERENCES v(i), i INT PRIMARY KEY);]]
        local _, err = box.execute(sql)
        local res = [[Failed to create foreign key constraint ]]..
            [['fk_unnamed_T_1': referenced space can't be VIEW]]
        t.assert_equals(err.message, res)
        box.execute([[DROP VIEW v;]])

        box.execute([[DROP TABLE t0;]])
    end)
end

-- Make sure tuple foreign key created properly.
g.test_constraints_4 = function()
    g.server:exec(function()
        local t = require('luatest')

        local sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT, ]]..
            [[FOREIGN KEY (i, a) REFERENCES t(i, a));]]
        box.execute(sql)
        local res = {fk_unnamed_T_1 = {space = box.space.T.id,
                                       field = {[1] = 1, [2] = 2}}}
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT, a INT, PRIMARY KEY(i, a), ]]..
            [[FOREIGN KEY (i, a) REFERENCES t);]]
        box.execute(sql)
        res = {fk_unnamed_T_1 = {space = box.space.T.id,
                                 field = {[1] = 1, [2] = 2}}}
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT, CONSTRAINT one ]]..
            [[FOREIGN KEY (i, a) REFERENCES t(i, a));]]
        box.execute(sql)
        res = {ONE = {space = box.space.T.id, field = {[1] = 1, [2] = 2}}}
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t0 (i INT, a INT, PRIMARY KEY (i, a));]])
        local space_id = box.space.T0.id

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT, ]]..
            [[FOREIGN KEY (i, a) REFERENCES t0(a, i));]]
        res = {fk_unnamed_T_1 = {space = space_id, field = {[1] = 2, [2] = 1}}}
        box.execute(sql)
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[DROP TABLE t;]])

        sql = [[CREATE TABLE t (i INT PRIMARY KEY, a INT, ]]..
            [[FOREIGN KEY (i, a) REFERENCES t0);]]
        res = {fk_unnamed_T_1 = {space = space_id, field = {[1] = 1, [2] = 2}}}
        box.execute(sql)
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[DROP TABLE t;]])

        box.execute([[CREATE TABLE t (i INT, a INT, PRIMARY KEY (i, a))]])

        sql = [[ALTER TABLE t ADD CONSTRAINT c FOREIGN KEY (a) REFERENCES t(i)]]
        res = {C = {space = box.space.T.id, field = {[2] = 1}}}
        box.execute(sql)
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[ALTER TABLE t DROP CONSTRAINT c;]])

        sql = [[ALTER TABLE t ADD CONSTRAINT c FOREIGN KEY (a, i) REFERENCES t]]
        res = {C = {space = box.space.T.id, field = {[2] = 1, [1] = 2}}}
        box.execute(sql)
        t.assert_equals(box.space.T.foreign_key, res)
        box.execute([[ALTER TABLE t DROP CONSTRAINT c;]])

        box.execute([[DROP TABLE t;]])

        box.execute([[DROP TABLE t0;]])
    end)
end
