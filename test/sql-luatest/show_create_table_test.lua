local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
    g.server:exec(function()
        rawset(_G, 'check_success', function(table_raw_name, res)
            local sql = 'SHOW CREATE TABLE '..table_raw_name..';'
            local ret = box.execute(sql)
            t.assert_equals(ret.rows[1][1], res)

            sql = 'SHOW CREATE TABLE '..table_raw_name..' INCLUDING ERRORS'
            ret = box.execute(sql)
            t.assert_equals(ret.rows[1][1], res)
        end)

        rawset(_G, 'check_error', function(table_raw_name, res, err)
            local sql = 'SHOW CREATE TABLE '..table_raw_name..';'
            local _, ret = box.execute(sql)
            t.assert_equals(ret.message, err)

            sql = 'SHOW CREATE TABLE '..table_raw_name..' INCLUDING ERRORS'
            ret = box.execute(sql)
            t.assert_equals(ret.rows[1][1], res)
        end)
    end)
end)

g.after_all(function()
    g.server:stop()
end)

g.test_show_create_table_one = function()
    g.server:exec(function()
        local _, err = box.execute('SHOW CREATE TABLE t INCLUDING ERRORS;')
        t.assert_equals(err.message, [[Space 'T' does not exist]])

        box.execute('CREATE TABLE t(i INT PRIMARY KEY, a INT);')
        local res = 'CREATE TABLE T(\nI INTEGER NOT NULL,\nA INTEGER,\n'..
                    'CONSTRAINT "pk_unnamed_T_1" PRIMARY KEY(I))\n'..
                    "WITH ENGINE = 'memtx';"
        _G.check_success('t', res)
        box.execute('DROP TABLE t;')

        local sql = 'CREATE TABLE T(\nI INTEGER NOT NULL,\n'..
                    "A STRING CONSTRAINT C2 CHECK(a != 'asd') "..
                    'CONSTRAINT C3 REFERENCES T(I),\n'..
                    'C UUID NOT NULL DEFAULT(uuid()),\n'..
                    'CONSTRAINT C0 PRIMARY KEY(I, C),\n'..
                    'CONSTRAINT C1 UNIQUE(A),\n'..
                    'CONSTRAINT C4 UNIQUE(A, C),\n'..
                    'CONSTRAINT C6 CHECK(i * a < 100),\n'..
                    'CONSTRAINT C5 FOREIGN KEY(I, A) '..
                    "REFERENCES T(A, C))\nWITH ENGINE = 'vinyl';"
        box.execute(sql)
        _G.check_success('t', sql)
        box.execute('DROP TABLE t;')

        -- Make sure SHOW, INCLUDING and ERRORS can be used as names.
        sql = [[CREATE TABLE show(including INT PRIMARY KEY, errors INT);]]
        local ret = box.execute(sql)
        t.assert(ret ~= nil);
        box.execute([[DROP TABLE show;]])
    end)
end

g.test_show_create_table_all = function()
    g.server:exec(function()
        local res = box.execute('SHOW CREATE TABLE;')
        t.assert_equals(res.rows, {})
        res = box.execute('SHOW CREATE TABLE INCLUDING ERRORS;')
        t.assert_equals(res.rows, {})

        box.execute('CREATE TABLE t1(i INT PRIMARY KEY, a INT);')
        box.execute('CREATE TABLE t2(i INT PRIMARY KEY, a INT);')
        box.execute('CREATE TABLE t3(i INT PRIMARY KEY, a INT);')
        local ret = box.execute('SHOW CREATE TABLE;')
        local res1 = 'CREATE TABLE T1(\nI INTEGER NOT NULL,\nA INTEGER,\n'..
                     'CONSTRAINT "pk_unnamed_T1_1" PRIMARY KEY(I))\n'..
                     "WITH ENGINE = 'memtx';"
        local res2 = 'CREATE TABLE T2(\nI INTEGER NOT NULL,\nA INTEGER,\n'..
                     'CONSTRAINT "pk_unnamed_T2_1" PRIMARY KEY(I))\n'..
                     "WITH ENGINE = 'memtx';"
        local res3 = 'CREATE TABLE T3(\nI INTEGER NOT NULL,\nA INTEGER,\n'..
                     'CONSTRAINT "pk_unnamed_T3_1" PRIMARY KEY(I))\n'..
                     "WITH ENGINE = 'memtx';"
        t.assert_equals(#ret.rows, 3)
        t.assert_equals(ret.rows[1][1], res1)
        t.assert_equals(ret.rows[2][1], res2)
        t.assert_equals(ret.rows[3][1], res3)

        ret = box.execute('SHOW CREATE TABLE INCLUDING ERRORS;')
        t.assert_equals(#ret.rows, 3)
        t.assert_equals(ret.rows[1][1], res1)
        t.assert_equals(ret.rows[2][1], res2)
        t.assert_equals(ret.rows[3][1], res3)

        box.schema.space.create('a')

        -- Make sure non-descriptive spaces are ignored by "SHOW CREATE TABLE;".
        ret = box.execute('SHOW CREATE TABLE;')
        t.assert_equals(#ret.rows, 3)
        t.assert_equals(ret.rows[1][1], res1)
        t.assert_equals(ret.rows[2][1], res2)
        t.assert_equals(ret.rows[3][1], res3)

        --
        -- Make sure "SHOW CREATE TABLE INCLUDING ERRORS;" show non-descriptive
        -- spaces.
        --
        ret = box.execute('SHOW CREATE TABLE INCLUDING ERRORS;')
        t.assert_equals(#ret.rows, 4)
        res = 'CREATE TABLE "a"(\n'..
              "/* Problem with space 'a': format is missing. */\n"..
              "/* Problem with space 'a': primary key is not defined. */)\n"..
              "WITH ENGINE = 'memtx';"
        t.assert_equals(ret.rows[1][1], res1)
        t.assert_equals(ret.rows[2][1], res2)
        t.assert_equals(ret.rows[3][1], res3)
        t.assert_equals(ret.rows[4][1], res)

        box.space.a:drop()
        box.execute('DROP TABLE t1;')
        box.execute('DROP TABLE t2;')
        box.execute('DROP TABLE t3;')
    end)
end

g.test_space_from_lua = function()
    g.server:exec(function()
        -- Working example.
        local s = box.schema.space.create('a', {format = {{'i', 'integer'}}})
        s:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)
        s:drop()

        -- No columns defined.
        s = box.schema.space.create('a');
        s:create_index('i', {parts = {{1, 'integer'}}})
        local err = "Problem with SQL description of space 'a': format is "..
                    "missing"
        res = 'CREATE TABLE "a"(\n'..
              "/* Problem with space 'a': format is missing. */\n"..
              "/* Problem with primary key 'i': field 1 is unnamed. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        -- No indexes defined.
        s = box.schema.space.create('a', {format = {{'i', 'integer'}}})
        err = "Problem with SQL description of space 'a': primary key is not "..
              "defined"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL\n'..
              "/* Problem with space 'a': primary key is not defined. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        -- Unsupported type of index.
        s = box.schema.space.create('a', {format = {{'i', 'integer'}}})
        s:create_index('i', {type = 'hash', parts = {{'i', 'integer'}}})
        err = "Problem with SQL description of space 'a': primary key has "..
              "unsupported index type"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL\n'..
              "/* Problem with space 'a': primary key has unsupported "..
              "index type. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        -- Parts of PK contains unnnamed columns.
        s = box.schema.space.create('a', {format = {{'i', 'integer'}}})
        s:create_index('i', {parts = {{2, 'integer'}}})
        err = "Problem with SQL description of primary key 'i': field 2 is "..
              "unnamed"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL\n'..
              "/* Problem with primary key 'i': field 2 is unnamed. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        -- Type of the part in PK different from type of the field.
        s = box.schema.space.create('a', {format = {{'i', 'integer'}}})
        s:create_index('i', {parts = {{'i', 'unsigned'}}})
        err = "Problem with SQL description of primary key 'i': field 'i' "..
              "and related part are of different types"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL\n'..
              "/* Problem with primary key 'i': field 'i' and related part "..
              "are of different types. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        -- Collation of the part in PK different from collation of the field.
        s = box.schema.space.create('a', {format = {{'i', 'string',
                                          collation = "unicode_ci"}}})
        s:create_index('i', {parts = {{'i', 'string', collation = "binary"}}})
        err = "Problem with SQL description of primary key 'i': field 'i' "..
              "and related part have different collations"
        res = 'CREATE TABLE "a"(\n"i" STRING COLLATE "unicode_ci" NOT NULL\n'..
              "/* Problem with primary key 'i': field 'i' and related part "..
              "have different collations. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s:drop()

        --
        -- Spaces with an engine other than "memtx" and "vinyl" cannot be
        -- created with CREATE TABLE.
        --
        err = "Problem with SQL description of index 'owner': non-unique index"
        res = 'CREATE TABLE "_vspace"(\n'..
              '"id" UNSIGNED NOT NULL,\n'..
              '"owner" UNSIGNED NOT NULL,\n'..
              '"name" STRING NOT NULL,\n'..
              '"engine" STRING NOT NULL,\n'..
              '"field_count" UNSIGNED NOT NULL,\n'..
              '"flags" MAP NOT NULL,\n'..
              '"format" ARRAY NOT NULL,\n'..
              'CONSTRAINT "primary" PRIMARY KEY("id"),\n'..
              'CONSTRAINT "name" UNIQUE("name")\n'..
              "/* Problem with index 'owner': non-unique index. */\n"..
              "/* Problem with space '_vspace': wrong space engine. */);"
        _G.check_error('"_vspace"', res, err)

        -- Make sure the table, field, and PK names are properly escaped.
        s = box.schema.space.create('"A"', {format = {{'"i', 'integer'}}})
        s:create_index('123', {parts = {{'"i', 'integer'}}})
        res = 'CREATE TABLE """A"""(\n"""i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "123" PRIMARY KEY("""i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"""A"""', res)
        s:drop()
    end)
end

g.test_field_foreign_key_from_lua = function()
    g.server:exec(function()
        local format = {{'i', 'integer'}}
        box.schema.space.create('a', {format = format})

        -- Working example.
        format[1].foreign_key = {a = {space = 'a', field = 'i'}}
        box.schema.space.create('b', {format = format})
        box.space.b:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL '..
                    'CONSTRAINT "a" REFERENCES "a"("i"),\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"b"', res)

        -- Wrong foreign field defined by id in foreign_key.
        format[1].foreign_key.a = {space = 'a', field = 5}
        box.space.b:format(format)
        local err = "Problem with SQL description of foreign key 'a': "..
                    "foreign field is unnamed"
        res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with foreign key 'a': foreign field is "..
              "unnamed. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"b"', res, err)

        -- Make sure field foreign key constraint name is properly escaped.
        format[1].foreign_key = {['"'] = {space = 'a', field = 'i'}}
        box.space.b:format(format)
        res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL '..
              'CONSTRAINT """" REFERENCES "a"("i"),\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"b"', res)

        box.space.b:drop()
        box.space.a:drop()
    end)
end

g.test_tuple_foreign_key_from_lua = function()
    g.server:exec(function()
        local opts = {format = {{'i', 'integer'}}}
        box.schema.space.create('a', opts)

        -- Working example.
        opts.foreign_key = {a = {space = 'a', field = {i = 'i'}}}
        box.schema.space.create('b', opts)
        box.space.b:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL,\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"),\n'..
                    'CONSTRAINT "a" FOREIGN KEY("i") REFERENCES "a"("i"))\n'..
                    "WITH ENGINE = 'memtx';"
        _G.check_success('"b"', res)

        -- Wrong foreign field defined by id in foreign_key.
        opts.foreign_key.a = {space = 'a', field = {[5] = 'i'}}
        box.space.b:alter(opts)
        local err = "Problem with SQL description of foreign key 'a': local "..
                    "field is unnamed"
        res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with foreign key 'a': local field is unnamed. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"b"', res, err)

        -- Wrong foreign field defined by id in foreign_key.
        opts.foreign_key.a = {space = 'a', field = {i = 5}}
        box.space.b:alter(opts)
        err = "Problem with SQL description of foreign key 'a': foreign "..
              "field is unnamed"
        res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with foreign key 'a': foreign field is "..
              "unnamed. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"b"', res, err)

        -- Make sure tuple foreign key constraint name is properly escaped.
        opts.foreign_key = {['a"b"c'] = {space = 'a', field = {i = 'i'}}}
        box.space.b:alter(opts)
        res = 'CREATE TABLE "b"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"),\n'..
              'CONSTRAINT "a""b""c" FOREIGN KEY("i") REFERENCES "a"("i"))\n'..
              "WITH ENGINE = 'memtx';"
        _G.check_success('"b"', res)

        box.space.b:drop()
        box.space.a:drop()
    end)
end

g.test_field_check_from_lua = function()
    g.server:exec(function()
        box.schema.func.create('f', {body = '"i" > 10', language = 'SQL_EXPR',
                                     is_deterministic = true})
        box.schema.func.create('f1', {body = 'function(a) return a > 10 end',
                                     is_deterministic = true})
        box.schema.func.create('f2', {body = '"b" > 10', language = 'SQL_EXPR',
                                     is_deterministic = true})

        -- Working example.
        local format = {{'i', 'integer', constraint = {a = 'f'}}}
        box.schema.space.create('a', {format = format})
        box.space.a:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL '..
                    'CONSTRAINT "a" CHECK("i" > 10),\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        -- Wrong function type.
        format[1].constraint.a = 'f1'
        box.space.a:format(format)
        local err = "Problem with SQL description of check constraint 'a': "..
                    "wrong constraint expression"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with check constraint 'a': wrong constraint "..
              "expression. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)

        -- Wrong field name in the function.
        format[1].constraint.a = 'f2'
        box.space.a:format(format)
        err = "Problem with SQL description of check constraint 'a': wrong "..
              "field name in constraint expression"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with check constraint 'a': wrong field name in "..
              "constraint expression. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)

        -- Make sure field check constraint name is properly escaped.
        format[1].constraint = {['""'] = 'f'}
        box.space.a:format(format)
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL '..
              'CONSTRAINT """""" CHECK("i" > 10),\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        box.space.a:drop()
        box.func.f:drop()
        box.func.f1:drop()
        box.func.f2:drop()
    end)
end

g.test_tuple_check_from_lua = function()
    g.server:exec(function()
        box.schema.func.create('f', {body = '"i" > 10', language = 'SQL_EXPR',
                                     is_deterministic = true})
        box.schema.func.create('f1', {body = 'function(a) return a > 10 end',
                                     is_deterministic = true})

        -- Working example.
        local opts = {format = {{'i', 'integer'}}, constraint = {a = 'f'}}
        box.schema.space.create('a', opts)
        box.space.a:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"),\n'..
                    'CONSTRAINT "a" CHECK("i" > 10))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        -- Wrong function type.
        opts.constraint.a = 'f1'
        box.space.a:alter(opts)
        local err = "Problem with SQL description of check constraint 'a': "..
                    "wrong constraint expression"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with check constraint 'a': wrong constraint "..
              "expression. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)

        -- Make sure tuple check constraint name is properly escaped.
        opts.constraint = {['"a"'] = 'f'}
        box.space.a:alter(opts)
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"),\n'..
              'CONSTRAINT """a""" CHECK("i" > 10))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        box.space.a:drop()
        box.func.f:drop()
        box.func.f1:drop()
    end)
end

g.test_wrong_collation = function()
    g.server:exec(function()
        local map = setmetatable({}, { __serialize = 'map' })
        local col_def = {'col1', 1, 'BINARY', '', map}
        local col = box.space._collation:auto_increment(col_def)
        t.assert(col ~= nil)

        -- Working example.
        local format = {{'i', 'string', collation = 'col1'}}
        box.schema.space.create('a', {format = format})
        local parts = {{'i', 'string', collation = 'col1'}}
        box.space.a:create_index('i', {parts = parts})
        local res = 'CREATE TABLE "a"(\n"i" STRING COLLATE "col1" NOT NULL,\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        -- Collations does not exists.
        box.space._collation:delete(col.id)
        local err = "Problem with SQL description of collation '277': "..
                    "collation does not exist"
        res = 'CREATE TABLE "a"(\n"i" STRING NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with collation '277': collation does not "..
              "exist. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)

        box.space._collation:insert(col)
        box.space.a:drop()
        box.space._collation:delete(col.id)

        -- Make sure collation name is properly escaped.
        col_def = {'"c"ol"2', 1, 'BINARY', '', map}
        col = box.space._collation:auto_increment(col_def)
        t.assert(col ~= nil)
        format = {{'i', 'string', collation = '"c"ol"2'}}
        box.schema.space.create('a', {format = format})
        parts = {{'i', 'string', collation = '"c"ol"2'}}
        box.space.a:create_index('i', {parts = parts})
        res = 'CREATE TABLE "a"(\n"i" STRING COLLATE """c""ol""2" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        box.space.a:drop()
        box.space._collation:delete(col.id)
    end)
end

g.test_index_from_lua = function()
    g.server:exec(function()
        local format = {{'i', 'integer'}, {'s', 'string', collation = 'binary'}}
        local s = box.schema.space.create('a', {format = format})
        s:create_index('i', {parts = {{'i', 'integer'}}})
        local res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
                    '"s" STRING COLLATE "binary" NOT NULL,\n'..
                    'CONSTRAINT "i" PRIMARY KEY("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        -- Working example.
        s:create_index('i1', {parts = {{'i', 'integer'}}})
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"),\nCONSTRAINT "i1" UNIQUE("i")'..
              ")\nWITH ENGINE = 'memtx';"
        _G.check_success('"a"', res)
        s.index.i1:drop()

        -- Unsupported type of the index.
        s:create_index('i1', {parts = {{'i', 'integer'}}, type = 'HASH'})
        local err = "Problem with SQL description of index 'i1': unsupported "..
                    "index type"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with index 'i1': unsupported index type. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s.index.i1:drop()

        -- Non-unique index.
        s:create_index('i1', {parts = {{'i', 'integer'}}, unique = false})
        err = "Problem with SQL description of index 'i1': non-unique index"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with index 'i1': non-unique index. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s.index.i1:drop()

        -- Parts contains an unnamed field.
        s:create_index('i1', {parts = {{5, 'integer'}}})
        err = "Problem with SQL description of index 'i1': field 5 is unnamed"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with index 'i1': field 5 is unnamed. */)\n"..
              "WITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s.index.i1:drop()

        -- Type of the part in index different from type of the field.
        s:create_index('i1', {parts = {{'i', 'unsigned'}}})
        err = "Problem with SQL description of index 'i1': field 'i' and "..
              "related part are of different types"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with index 'i1': field 'i' and related part are of "..
              "different types. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s.index.i1:drop()

        -- Collation of the part in index different from collation of the field.
        s:create_index('i1', {parts = {{'s', 'string', collation = "unicode"}}})
        err = "Problem with SQL description of index 'i1': field 's' and "..
              "related part have different collations"
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i")\n'..
              "/* Problem with index 'i1': field 's' and related part have "..
              "different collations. */)\nWITH ENGINE = 'memtx';"
        _G.check_error('"a"', res, err)
        s.index.i1:drop()

        -- Make sure index name is properly escaped.
        s:create_index('i7"', {parts = {{'i', 'integer'}}})
        res = 'CREATE TABLE "a"(\n"i" INTEGER NOT NULL,\n'..
              '"s" STRING COLLATE "binary" NOT NULL,\n'..
              'CONSTRAINT "i" PRIMARY KEY("i"),\n'..
              'CONSTRAINT "i7""" UNIQUE("i"))\nWITH ENGINE = \'memtx\';'
        _G.check_success('"a"', res)

        s:drop()
    end)
end
