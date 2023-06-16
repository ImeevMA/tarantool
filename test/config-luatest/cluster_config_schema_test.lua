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

g.test_example_credentials = function()
    local config_file = fio.abspath('doc/examples/config/credentials.yaml')
    local fh = fio.open(config_file, {'O_RDONLY'})
    local config = yaml.decode(fh:read())
    fh:close()
    cluster_config:validate(config)
end

-- TODO: Enable these test cases closer to the 3.0.0 release, when
-- the schema will be frozen.
--[[
local bad_config_cases = {
    -- Verify config.version.
    no_config = {
        config = {},
        err = '[cluster_config] config.version is mandatory',
    },
    no_config_version = {
        config = {config = {}},
        err = '[cluster_config] config.version is mandatory',
    },
    unknown_config_version = {
        config = {config = {version = '0.0.0'}},
        err = '[cluster_config] config.version: Got 0.0.0, but only the ' ..
            'following values are allowed: 3.0.0'
    },
    config_version_in_group_scope = {
        config = {
            config = {
                version = '3.0.0',
            },
            groups = {
                ['group-001'] = {
                    config = {
                        version = '3.0.0',
                    },
                    replicasets = {
                        ['replicaset-001'] = {
                            instances = {
                                ['instance-001'] = {},
                            },
                        },
                    },
                },
            },
        },
        err = '[cluster_config] groups.group-001: config.version must not ' ..
            'be present in the group scope',
    },
    config_version_in_replicaset_scope = {
        config = {
            config = {
                version = '3.0.0',
            },
            groups = {
                ['group-001'] = {
                    replicasets = {
                        ['replicaset-001'] = {
                            config = {
                                version = '3.0.0',
                            },
                            instances = {
                                ['instance-001'] = {},
                            },
                        },
                    },
                },
            },
        },
        err = '[cluster_config] groups.group-001.replicasets.' ..
            'replicaset-001: config.version must not be present in the ' ..
            'replicaset scope',
    },
    config_version_in_instance_scope = {
        config = {
            config = {
                version = '3.0.0',
            },
            groups = {
                ['group-001'] = {
                    replicasets = {
                        ['replicaset-001'] = {
                            instances = {
                                ['instance-001'] = {
                                    config = {
                                        version = '3.0.0',
                                    },
                                },
                            },
                        },
                    },
                },
            },
        },
        err = '[cluster_config] groups.group-001.replicasets.replicaset-001.' ..
            'instances.instance-001: config.version must not be present in ' ..
            'the instance scope',
    },
}

for case_name, case in pairs(bad_config_cases) do
    g['test_' .. case_name] = function()
        t.assert_error_msg_equals(case.err, function()
            cluster_config:validate(case.config)
        end)
    end
end
]]--
