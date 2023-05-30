local schema = require('conf.utils.schema')
local instance_config = require('conf.instance_config')

local values = {}

local function sync(_ctx, _iconfig)
    for _, w in instance_config:pairs() do
        local env_var_name = 'TT_' .. table.concat(w.path, '_'):upper()
        local raw_value = os.getenv(env_var_name)
        local value = schema.fromenv(env_var_name, raw_value, w.schema)
        if value ~= nil then
            instance_config:set(values, w.path, value)
        end
    end
end

local function get()
    return values
end

local function info()
    return 'Environment'
end

return {
    name = 'env',
    -- The type is either 'instance' or 'cluster'.
    type = 'instance',
    -- Gather most actual config values.
    sync = sync,
    -- Access the configuration after source.sync().
    --
    -- source.get()
    get = get,
    -- Information about the source.
    info = info,
}
