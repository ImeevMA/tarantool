-- Accumulates configuration data of different kinds and provides
-- accessors.
--
-- Intended to be used as an immutable object.

local fun = require('fun')
local instance_config = require('conf.instance_config')
local cluster_config = require('conf.cluster_config')

local function choose_iconfig(self, opts)
    if opts ~= nil and opts.peer ~= nil then
        local peers = rawget(self, '_peers')
        local peer = peers[opts.peer]
        if peer == nil then
            error(('Unknown peer %q'):format(opts.peer))
        end
        if opts ~= nil and opts.use_default then
            return peer.iconfig_def
        end
        return peer.iconfig
    end

    if opts ~= nil and opts.use_default then
        return rawget(self, '_iconfig_def')
    else
        return rawget(self, '_iconfig')
    end
end

local methods = {}

-- Acquire a value from the instance config.
--
-- opts:
--     use_default: boolean
--     peer: string
function methods.get(self, path, opts)
    local data = choose_iconfig(self, opts)
    -- NB: use_default is handled in choose_iconfig(), so it is
    -- not passed down.
    return instance_config:get(data, path)
end

-- Filter data based on the instance schema annotations.
--
-- opts:
--     use_default: boolean
--     peer: string
function methods.filter(self, f, opts)
    local data = choose_iconfig(self, opts)
    -- NB: use_default is handled in choose_iconfig(), so it is
    -- not passed down.
    return instance_config:filter(data, f)
end

-- List of names of the instances in the same replicaset.
--
-- The names are useful to pass to other methods as opts.peer.
function methods.peers(self)
    return rawget(self, '_peer_names')
end

local mt = {
    __index = methods,
}

local function apply_vars_f(schema, data, w, vars)
    if schema.type == 'string' and data ~= nil then
        assert(type(data) == 'string')
        return (data:gsub('{{ *(.-) *}}', function(var_name)
            if vars[var_name] ~= nil then
                return vars[var_name]
            end
            w.error(('Unknown variable %q'):format(var_name))
        end))
    end
    return data
end

local function iconfig_apply_vars(iconfig, vars)
    return instance_config:map(iconfig, apply_vars_f, vars)
end

local function new(iconfig, cconfig, instance_name)
    -- Precalculate configuration with applied defaults.
    local iconfig_def = instance_config:apply_default(iconfig)

    -- Substitute {{ instance_name }} with actual instance name in
    -- the original config and in the config with defaults.
    local vars = {instance_name = instance_name}
    iconfig = iconfig_apply_vars(iconfig, vars)
    iconfig_def = iconfig_apply_vars(iconfig_def, vars)

    -- Find myself in a cluster config, determine peers in the same
    -- replicaset and store instance configs for the peers.
    local peers = {}
    local found = cluster_config:find_instance(cconfig, instance_name)
    if found ~= nil then
        for peer_name, _ in pairs(found.replicaset.instances) do
            -- Build config for each peer from the cluster config.
            -- Build a config with applied defaults as well.
            local peer_iconfig = cluster_config:instantiate(cconfig, peer_name)
            local peer_iconfig_def = instance_config:apply_default(peer_iconfig)

            -- Substitute variables according to the instance name
            -- of the peer.
            local peer_vars = {instance_name = peer_name}
            peer_iconfig = iconfig_apply_vars(peer_iconfig, peer_vars)
            peer_iconfig_def = iconfig_apply_vars(peer_iconfig_def, peer_vars)

            peers[peer_name] = {
                iconfig = peer_iconfig,
                iconfig_def = peer_iconfig_def,
            }
        end
    end

    return setmetatable({
        _iconfig = iconfig,
        _iconfig_def = iconfig_def,
        _cconfig = cconfig,
        _peer_names = fun.iter(peers):totable(),
        _peers = peers,
        _instance_name = instance_name,
    }, mt)
end

return {
    new = new,
}
