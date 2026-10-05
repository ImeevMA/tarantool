local server = require('luatest.server')
local t = require('luatest')

local g = t.group("alter_table", {{engine = 'memtx'}, {engine = 'vinyl'}})

g.before_all(function(cg)
    cg.server = server:new({alias = 'master'})
    cg.server:start()
    cg.server:exec(function(engine)
        local sql = [[SET SESSION "sql_default_engine" = '%s';]]
        box.execute(sql:format(engine))
    end, {cg.params.engine})
end)

g.after_all(function(cg)
    cg.server:drop()
end)

--
-- Make sure there are no issues with index deletion after renaming the space.
-- Also, verify that index creation and deletion are being persistenly
-- preserved.
--
g.test_3613_idx_alter_update = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (s1 INT PRIMARY KEY);]])
        box.execute([[CREATE INDEX i ON t (s1);]])
        box.execute([[ALTER TABLE t RENAME TO j3;]])

        -- Due to gh-3613, next stmt caused segfault.
        box.execute([[DROP INDEX i ON j3;]])

        -- Make sure that no artifacts remain after restart.
        box.snapshot()
    end)
    cg.server:restart()
    cg.server:exec(function()
        local exp_err = [[No index 'i' is defined in space 'j3']]
        local _, err = box.execute([[DROP INDEX i ON j3;]])
        t.assert_equals(tostring(err), exp_err)

        box.execute([[CREATE INDEX i ON j3 (s1);]])

        -- Check that _index was altered properly.
        box.snapshot()
    end)
    cg.server:restart()
    cg.server:exec(function()
        local _, err = box.execute([[DROP INDEX i ON j3;]])
        t.assert_equals(err, nil)

        -- Cleanup.
        box.execute([[DROP TABLE j3;]])
    end)
end

--
-- After gh-3613 fix, bug in cmp_def was discovered.
-- Comparison didn't take .opts.sql into account
-- marking corresponding xlog entry useless.
--
g.test_3613_idx_alter_update_2 = function(cg)
    cg.server:exec(function()
        box.execute('CREATE TABLE t (s1 INT PRIMARY KEY);')
        box.execute('CREATE INDEX i ON t (s1);')
        local res = box.execute('ALTER TABLE t RENAME TO j3;')
        t.assert_equals(res, {row_count = 0})
    end)
    cg.server:restart()
    cg.server:exec(function()
        box.execute('DROP INDEX i ON j3;')
        -- Cleanup.
        box.execute('DROP TABLE j3;')
    end)
end

--
-- Check the foreign field ID for a field foreign key
-- not lost after executing an `ALTER TABLE ADD COLUMN` statement.
--
g.test_13010_repeat_adding_constraint_column = function(cg)
    cg.server:exec(function()
        box.execute("CREATE TABLE t(i INT PRIMARY KEY, a INT REFERENCES t(i));")
        local exp = {fk_unnamed_t_a_1 = {space = box.space.t.id, field = 1}}
        t.assert_equals(box.space.t:format()[2]['foreign_key'], exp)
        box.execute("ALTER TABLE t ADD COLUMN b;")
        t.assert_equals(box.space.t:format()[2]['foreign_key'], exp)
    end)
end

-- gh-5485: Check changed errors for `ALTER TABLE` rules.
g.test_alter_table_reserved_keyword_error = function(cg)
    cg.server:exec(function()
        box.execute([[CREATE TABLE t (id INT PRIMARY KEY, a INT);]])

        local exp_err = "Syntax error at line 1 near 'REFERENCES'"
        local queries = {
            [[ALTER TABLE t ADD COLUMN REFERENCES INT;]],
            [[ALTER TABLE t ADD REFERENCES INT;]],
            [[ALTER TABLE t ADD CONSTRAINT fk FOREIGN KEY REFERENCES t(id);]],
            [[ALTER TABLE t ADD CONSTRAINT ck CHECK REFERENCES;]],
            [[ALTER TABLE t ADD CONSTRAINT uq UNIQUE REFERENCES;]],
            [[ALTER TABLE t ADD CONSTRAINT pk PRIMARY KEY REFERENCES;]],
        }
        for _, query in ipairs(queries) do
            local _, err = box.execute(query)
            t.assert_equals(err.message, exp_err)
        end

        box.space.t:drop()
    end)
end

--
-- Make sure ALTER TABLE ADD CONSTRAINT reports errors in the same order as
-- before the resolving of the statement was separated from the code
-- generation.
--
g.test_add_constraint_check_order = function(cg)
    cg.server:exec(function()
        local function check(sql, exp)
            local _, err = box.execute(sql)
            t.assert_equals(err.message, exp, sql)
        end
        box.execute([[CREATE TABLE p (a INT PRIMARY KEY, "B" INT);]])
        box.execute([[CREATE TABLE t (a INT PRIMARY KEY, "B" INT, c INT);]])
        box.execute([[CREATE VIEW v AS SELECT * FROM t;]])
        box.schema.space.create('nopk', {format = {{'a', 'integer'}}})

        -- UNIQUE and PRIMARY KEY are checked as CREATE INDEX is.
        check([[ALTER TABLE nosuch ADD CONSTRAINT u UNIQUE (a COLLATE x);]],
              "Collation 'x' does not exist")
        check([[ALTER TABLE nosuch ADD CONSTRAINT u PRIMARY KEY
                (a AUTOINCREMENT, b AUTOINCREMENT);]],
              "Table must feature at most one AUTOINCREMENT field")
        check([[ALTER TABLE nosuch ADD CONSTRAINT u UNIQUE (nosuch);]],
              "Space 'nosuch' does not exist")
        check([[ALTER TABLE v ADD CONSTRAINT u UNIQUE (nosuch);]],
              "Views can not be indexed")
        check([[ALTER TABLE t ADD CONSTRAINT "" UNIQUE (nosuch);]],
              "Invalid identifier '' (expected printable symbols only " ..
              "or it is too long)")
        check([[ALTER TABLE t ADD CONSTRAINT u UNIQUE (a, nosuch);]],
              "Can't resolve field 'nosuch'")

        -- FOREIGN KEY.
        local inv = "Invalid identifier '' (expected printable symbols " ..
                    "only or it is too long)"
        check([[ALTER TABLE nosuch ADD CONSTRAINT f FOREIGN KEY ("")
                REFERENCES nosuch;]], inv)
        check([[ALTER TABLE nosuch ADD CONSTRAINT f FOREIGN KEY (a)
                REFERENCES nosuch ("");]], inv)
        check([[ALTER TABLE nosuch ADD CONSTRAINT f FOREIGN KEY (a)
                REFERENCES nosuch2;]], "Space 'nosuch' does not exist")
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (a)
                REFERENCES nosuch;]], "Space 'nosuch' does not exist")
        local err = "Failed to create foreign key constraint 'f': "
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (nosuch)
                REFERENCES v;]], err .. "referenced space can't be VIEW")
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (nosuch)
                REFERENCES nopk;]],
              err .. "referenced space doesn't feature PRIMARY KEY")
        local mismatch = err .. "number of columns in foreign key does " ..
                         "not match the number of columns in the primary " ..
                         "index of referenced table"
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (a, nosuch)
                REFERENCES p;]], mismatch)
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (nosuch)
                REFERENCES p (a, nosuch);]], mismatch)
        local nofield = err .. "foreign key refers to nonexistent field "
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (a, nosuch)
                REFERENCES p (nosuch2, a);]], nofield .. "nosuch2")
        check([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (a, nosuch)
                REFERENCES p (a, b);]], nofield .. "nosuch")

        -- CHECK: only the space is resolved.
        check([[ALTER TABLE nosuch ADD CONSTRAINT c CHECK (nosuch);]],
              "Space 'nosuch' does not exist")

        -- Unquoted names have legacy form.
        box.execute([[ALTER TABLE t ADD CONSTRAINT u UNIQUE (b, a);]])
        local parts = box.space.t.index.u.parts
        t.assert_equals({parts[1].fieldno, parts[2].fieldno}, {2, 1})
        box.execute([[ALTER TABLE t ADD CONSTRAINT f FOREIGN KEY (c, b)
                      REFERENCES p (b, a);]])
        t.assert_equals(box.space.t.foreign_key.f,
                        {space = box.space.p.id, field = {[3] = 2, [2] = 1}})
        box.execute([[ALTER TABLE t ADD CONSTRAINT f2 FOREIGN KEY (c)
                      REFERENCES p;]])
        t.assert_equals(box.space.t.foreign_key.f2,
                        {space = box.space.p.id, field = {[3] = 1}})
        box.execute([[ALTER TABLE t ADD CONSTRAINT c CHECK (a > 0);]])
        t.assert_equals(box.space.t.constraint.c, box.func.check_t_c.id)
        box.execute([[ALTER TABLE nopk ADD CONSTRAINT pk PRIMARY KEY
                      (a AUTOINCREMENT);]])
        t.assert_equals(box.space.nopk:insert({box.NULL}), {1})

        box.space.nopk:drop()
        box.execute([[DROP VIEW v;]])
        box.execute([[DROP TABLE t;]])
        box.execute([[DROP TABLE p;]])
        box.func.check_t_c:drop()
    end)
end
