local t = require('luatest')
local instance_config = require('internal.config.instance_config')

local g = t.group()

g.test_basic = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
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
        database = {
            rw = true,
        },
    }
    instance_config:validate(iconfig)
end

local bad_config_cases = {
    -- Verify config.version.
    no_config = {
        iconfig = {},
        err = '[instance_config] config.version is mandatory',
    },
    no_config_version = {
        iconfig = {config = {}},
        err = '[instance_config] config.version is mandatory',
    },
    unknown_config_version = {
        iconfig = {config = {version = '0.0.0'}},
        err = '[instance_config] config.version: Got 0.0.0, but only the ' ..
            'following values are allowed: 3.0.0'
    },
}

for case_name, case in pairs(bad_config_cases) do
    g['test_' .. case_name] = function()
        t.assert_error_msg_equals(case.err, function()
            instance_config:validate(case.iconfig)
        end)
    end
end
