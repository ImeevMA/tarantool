local json = require('json')
local instance_config = require('conf.instance_config')

local values = {}

-- XXX: Move string (from env) -> Lua value transformation to a
-- schema.
local function transform_from_env(env_var_name, node_type, scalar_type)
    local raw_value = os.getenv(env_var_name)

    if raw_value == nil or raw_value == '' then
        return nil
    end

    if node_type == 'map' then
        -- JSON.
        if raw_value:startswith('{') then
            local ok, res = pcall(json.decode, raw_value)
            if not ok then
                error(('Unable to decode JSON data in environment ' ..
                    'variable %q: %s'):format(env_var_name, res))
            end
            return res
        end

        -- foo=bar,baz=fiz
        local res = {}
        for _, v in ipairs(raw_value:split(',')) do
            local eq = v:find('=')
            if eq == nil then
                error(('Expected JSON or foo=bar,fiz=baz format for environmnt ' ..
                    'variable %q'):format(env_var_name))
            end
            local lhs = string.sub(v, 1, eq - 1)
            local rhs = string.sub(v, eq + 1)

            if lhs == '' then
                error(('Expected JSON or foo=bar,fiz=baz format for environmnt ' ..
                    'variable %q'):format(env_var_name))
            end
            res[lhs] = tonumber(rhs) or rhs
        end
        return res
    end

    assert(node_type == 'scalar')

    if scalar_type == 'string' then
        return raw_value
    elseif scalar_type == '[string]' then
        return raw_value:split(',')
    elseif scalar_type == 'integer' then
        -- XXX: Forbid floats.
        return tonumber64(raw_value)
    elseif scalar_type == 'number' then
        -- XXX: Support large integers?
        return tonumber(raw_value)
    elseif scalar_type == 'number, string' or
            scalar_type == 'string, number' then -- XXX: just hack
        return tonumber(raw_value) or raw_value
    elseif scalar_type == 'boolean' then
        -- Accept false/true case insensitively.
        --
        -- Accept 0/1 as boolean values.
        if raw_value:lower() == 'false' or raw_value == '0' then
            return false
        end
        if raw_value:lower() == 'true' or raw_value == '1' then
            return true
        end

        error(('Unable to decode boolean value from environment ' ..
            'variable %q'):format(env_var_name))
    end

    error(('Unknown environment option type: %q'):format(scalar_type))
end

local function sync(_ctx)
    for _, w in instance_config:pairs() do
        local node_type = w.type
        local scalar_type = w.type == 'scalar' and w.schema.type or nil
        local env_var_name = 'TT_' .. table.concat(w.path, '_'):upper()
        local value = transform_from_env(env_var_name, node_type, scalar_type)
        if value ~= nil then
            instance_config:set(values, w.path, value)
        end
    end
end

local function get()
    return values
end

return {
    name = 'env',
    -- doc/source_type.md
    type = 'instance',
    -- Gather most actual config values.
    sync = sync,
    -- Access the configuration after source.sync().
    --
    -- source.get()
    get = get,
}
