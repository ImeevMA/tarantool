local yaml = require('yaml')
local fio = require('fio')
local t = require('luatest')
local cluster_config = require('internal.config.cluster_config')

local g = t.group()

g.test_basic = function()
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
    cluster_config:validate(config)
end

g.test_example_single = function()
    local config_file = fio.abspath('doc/examples/config/single.yaml')
    local fh = fio.open(config_file, {'O_RDONLY'})
    local config = yaml.decode(fh:read())
    fh:close()
    cluster_config:validate(config)
end

g.test_example_replicaset = function()
    local config_file = fio.abspath('doc/examples/config/replicaset.yaml')
    local fh = fio.open(config_file, {'O_RDONLY'})
    local config = yaml.decode(fh:read())
    fh:close()
    cluster_config:validate(config)
end
