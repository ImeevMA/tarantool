local schema = require('conf.utils.schema')
local instance_config = require('conf.instance_config')

local function find_instance(_schema, data, instance_name)
    -- Find group, replicaset, instance configuration for the
    -- given instance.
    local groups = data.groups or {}
    for gn, g in pairs(groups) do
        local replicasets = g.replicasets or {}
        for rn, r in pairs(replicasets) do
            local instances = r.instances or {}
            if instances[instance_name] ~= nil then
                return {
                    group = g,
                    replicaset = r,
                    instance = instances[instance_name],
                    group_name = gn,
                    replicaset_name = rn,
                }
            end
        end
    end

    return nil
end

local function instantiate(_schema, data, instance_name)
    -- No topology information provided.
    if data.groups == nil then
        return data
    end

    local found = find_instance(nil, data, instance_name)

    if found == nil then
        local res = table.copy(data)
        res.groups = nil
        return res
    end

    local res = {}
    res = instance_config:merge(res, data)
    res.groups = nil
    res = instance_config:merge(res, found.group)
    res.replicasets = nil
    res = instance_config:merge(res, found.replicaset)
    res.instances = nil
    res = instance_config:merge(res, found.instance)
    return res
end

local instances = schema.map({
    key = schema.scalar({type = 'string'}),
    value = schema.annotate(instance_config, {scope = 'instance'}),
})

local replicasets = schema.map({
    key = schema.scalar({type = 'string'}),
    value = schema.mix(
        schema.annotate(instance_config, {scope = 'replicaset'}),
        {instances = instances}
    ),
})

local groups = schema.map({
    key = schema.scalar({type = 'string'}),
    value = schema.mix(
        schema.annotate(instance_config, {scope = 'group'}),
        {replicasets = replicasets}
    ),
})

return schema.new('cluster_config', schema.mix(
    schema.annotate(instance_config, {scope = 'global'}),
    {groups = groups}
), {
    methods = {
        instantiate = instantiate,
        find_instance = find_instance,
    },
})
