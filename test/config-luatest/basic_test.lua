local fun = require('fun')
local yaml = require('yaml')
local fio = require('fio')
local t = require('luatest')
local treegen = require('test.treegen')
local server = require('test.luatest_helpers.server')

local g = t.group()

g.before_all(function(g)
    treegen.init(g)
end)

g.after_all(function(g)
    treegen.clean(g)
end)

g.after_each(function(g)
    for k, v in pairs(g) do
        if k == 'server' or k:match('^server_%d+$') then
            v:stop()
        end
    end
end)

g.test_basic = function(g)
    local dir = treegen.prepare_directory(g, {}, {})
    local config = {
        credentials = {
            users = {
                guest = {
                    roles = {'super'},
                },
            },
        },
        iproto = {
            listen = 'unix/:./{{ instance_name }}.iproto',
        },
        groups = {
            ['group-001'] = {
                replicasets = {
                    ['replicaset-001'] = {
                        instances = {
                            ['instance-001'] = {
                                database = {
                                    rw = true,
                                },
                            },
                        },
                    },
                },
            },
        },
    }
    local config_file = treegen.write_script(dir, 'config.yaml', yaml.encode(config))
    local opts = {config_file = config_file, chdir = dir}
    g.server = server:new(fun.chain(opts, {alias = 'instance-001'}):tomap())
    g.server:start()
    t.assert_equals(g.server:eval('return box.info.name'), g.server.alias)
end

g.test_example_single = function()
    local dir = treegen.prepare_directory(g, {}, {})
    local config_file = fio.abspath('doc/examples/config/single.yaml')
    local opts = {config_file = config_file, chdir = dir}
    g.server = server:new(fun.chain(opts, {alias = 'instance-001'}):tomap())
    g.server:start()
    t.assert_equals(g.server:eval('return box.info.name'), g.server.alias)
end

g.test_example_replicaset = function()
    local dir = treegen.prepare_directory(g, {}, {})
    local config_file = fio.abspath('doc/examples/config/replicaset.yaml')

    local credentials = {
        user = 'client',
        password = 'secret',
    }
    local opts = {config_file = config_file, chdir = dir,
        net_box_credentials = credentials}
    g.server_1 = server:new(fun.chain(opts, {alias = 'instance-001'}):tomap())
    g.server_2 = server:new(fun.chain(opts, {alias = 'instance-002'}):tomap())
    g.server_3 = server:new(fun.chain(opts, {alias = 'instance-003'}):tomap())

    g.server_1:start({wait_until_ready = false})
    g.server_2:start({wait_until_ready = false})
    g.server_3:start({wait_until_ready = false})

    g.server_1:wait_until_ready()
    g.server_2:wait_until_ready()
    g.server_3:wait_until_ready()

    local info = g.server_1:eval('return box.info')
    t.assert_equals(info.name, 'instance-001')
    t.assert_equals(info.replicaset.name, 'replicaset-001')
    t.assert_equals(info.cluster.name, 'group-001')

    -- box.info.name is not set on a read-only instance.
    local info = g.server_2:eval('return box.info')
    t.assert_equals(info.replicaset.name, 'replicaset-001')
    t.assert_equals(info.cluster.name, 'group-001')

    -- box.info.name is not set on a read-only instance.
    local info = g.server_3:eval('return box.info')
    t.assert_equals(info.replicaset.name, 'replicaset-001')
    t.assert_equals(info.cluster.name, 'group-001')
end
