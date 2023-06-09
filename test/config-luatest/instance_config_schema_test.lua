local t = require('luatest')
local instance_config = require('internal.config.cluster_config')

local g = t.group()

g.test_basic = function()
    local iconfig = {
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
