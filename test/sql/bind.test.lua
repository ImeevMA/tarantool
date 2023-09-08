netbox = require('net.box')
test_run = require('test_run').new()
box.execute([[SET SESSION "sql_seq_scan" = true;]])

box.execute('CREATE TABLE test (id INT PRIMARY KEY, a NUMBER, b TEXT)')
box.space.test:replace{1, 2, '3'}
box.space.test:replace{7, 8.5, '9'}
box.space.test:replace{10, 11, box.NULL}

remote = test_run:get_cfg('remote') == 'true'
execute = nil
test_run:cmd("setopt delimiter ';'")
if remote then
	box.schema.user.grant('guest','read, write, execute', 'universe')
	box.schema.user.grant('guest', 'create', 'space')
	cn = netbox.connect(box.cfg.listen)
	execute = function(...) return cn:execute(...) end
else
	execute = function(...)
		local res, err = box.execute(...)
		if err ~= nil then
			error(err)
		end
		return res
	end
end;
test_run:cmd("setopt delimiter ''");
--
-- gh-3401: box.execute parameter binding.
--
parameters = {}
parameters[1] = {}
parameters[1][':value'] = 1
execute('SELECT * FROM test WHERE id = :value', parameters)
execute('SELECT ?, ?, ?', {1, 2, 3})
parameters = {}
parameters[1] = 10
parameters[2] = {}
parameters[2]['@value2'] = 12
parameters[3] = {}
parameters[3][':value1'] = 11
execute('SELECT ?, :value1, @value2', parameters)

parameters = {}
parameters[1] = {}
parameters[1][':value3'] = 1
parameters[2] = 2
parameters[3] = {}
parameters[3][':value1'] = 3
parameters[4] = 4
parameters[5] = 5
parameters[6] = {}
parameters[6]['@value2'] = 6
execute('SELECT :value3, ?, :value1, ?, ?, @value2, ?, :value3', parameters)

-- Try not-integer types.
msgpack = require('msgpack')
execute('SELECT ?, ?, ?, ?, ?', {'abc', -123.456, msgpack.NULL, true, false})

-- Try to replace '?' in meta with something meaningful.
execute('SELECT ? AS kek, ? AS kek2', {1, 2})

-- Try to bind not existing name.
parameters = {}
parameters[1] = {}
parameters[1]['name'] = 300
execute('SELECT ? AS kek', parameters)

-- Try too many parameters in a statement.
sql = 'SELECT '..string.rep('?, ', box.schema.SQL_BIND_PARAMETER_MAX)..'?'
execute(sql)

-- Try too many parameter values.
sql = 'SELECT ?'
parameters = {}
for i = 1, box.schema.SQL_BIND_PARAMETER_MAX + 1 do parameters[i] = i end
execute(sql, parameters)

--
-- Errors during parameters binding.
--
-- Try value > INT64_MAX. sql can't bind it, since it has no
-- suitable method in its bind API.
execute('SELECT ? AS big_uint', {0xefffffffffffffff})
-- Bind incorrect parameters.
parameters = {}
parameters[1] = {}
parameters[1][100] = 200
ok, err = pcall(execute, 'SELECT ?', parameters)
ok

parameters = {}
parameters[1] = {}
parameters[1][':value'] = {kek = 300}
execute('SELECT :value', parameters)

-- gh-3810: bind values of integer in range up to 2^64 - 1.
--
execute('SELECT ? ', {18446744073709551615ULL})

-- Make sure that VARBINARY values can be bound. Note that
-- at the moment there's no direct way to encode value as MP_BIN,
-- so we have to use workaround only with remote option.
--
test_run:cmd("setopt delimiter ';'")

if remote then
	execute("CREATE TABLE t(a VARBINARY PRIMARY KEY);")
	execute("INSERT INTO t VALUES (X'00');")
	res = execute("SELECT TYPEOF(?);", box.space.t:select()[1])
	assert(res['rows'][1][1] == "varbinary")
	execute("DROP TABLE t;")
end;

if remote then
	cn:close()
	box.schema.user.revoke('guest', 'read, write, execute', 'universe')
	box.schema.user.revoke('guest', 'create', 'space')
end;
test_run:cmd("setopt delimiter ''");

box.execute('DROP TABLE test')

box.execute('SELECT ?', {1, 2})
box.execute('SELECT $2', {1, 2, 3})

-- gh-4566: bind variable to LIKE argument resulted to crash.
--
box.execute("CREATE TABLE t (id INT PRIMARY KEY, a TEXT);")
box.execute("SELECT * FROM t WHERE a LIKE ?;", {'a%'});
box.execute("INSERT INTO t VALUES (1, 'aA'), (2, 'Ba'), (3, 'A');")
box.execute("SELECT * FROM t WHERE a LIKE ?;", {'a%'});

box.space.t:drop()
