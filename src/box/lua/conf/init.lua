local instance_config = require('conf.instance_config')
local cluster_config = require('conf.cluster_config')
local configdata = require('conf.configdata')

-- conf module context.
local ctx = {
    sources = {},
    appliers = {},
    -- There are values the module need to hold, which are not
    -- part of the configuration. They're stored here.
    instance_name = nil,
    config_file = nil,
    -- Applied config values.
    --
    -- XXX: Split gathered values, applied values.
    configdata = nil,
}

local function register_source(source)
    assert(type(source) == 'table')
    if source.type ~= 'instance' and source.type ~= 'cluster' then
        error("[source %q] source.type must be 'instance' or 'cluster'",
            tostring(source.type))
    end
    assert(source.sync ~= nil)
    assert(source.get ~= nil)
    table.insert(ctx.sources, source)
end

local function register_applier(applier)
    table.insert(ctx.appliers, applier)
end

local function parse_args(instance_name, config_file)
    if instance_name == nil then
        instance_name = os.getenv('TT_INSTANCE_NAME')
    end

    if instance_name == nil then
        error('No instance name provided')
    end

    if config_file == nil then
        config_file = os.getenv('TT_CONFIG')
    end

    ctx.instance_name = instance_name
    ctx.config_file = config_file
end

local function initialize()
    register_source(require('conf.source.env'))

    if ctx.config_file ~= nil then
        register_source(require('conf.source.file'))
    end

    register_applier(require('conf.applier.mkdir'))
    register_applier(require('conf.applier.box_cfg'))
    register_applier(require('conf.applier.console'))
    register_applier(require('conf.applier.fiber'))
end

local function collect()
    -- XXX: Should order of sync should be the same as order of
    -- values merge?
    for _, source in ipairs(ctx.sources) do
        -- Gather config values.
        --
        -- XXX: Error handling.
        -- Idea: add ctx.error(). We can set it to error() at
        -- first and to add errors into some table later.
        --
        -- XXX: We need a revision from a source?
        source.sync(ctx)
    end

    -- Validate configurations gathered from the sources.
    for _, source in ipairs(ctx.sources) do
        if source.type == 'instance' then
            instance_config:validate(source.get())
        elseif source.type == 'cluster' then
            cluster_config:validate(source.get())
        else
            assert(false)
        end
    end

    -- TODO: We need specific validators at least for the instance
    -- config. For example, there should be a validator, which
    -- forbids Tarantool EE options in Tarantool CE.

    -- XXX: If config section changes, we may want to attach and/or
    -- reconfigure sources.

    -- Merge configurations from the sources.
    --
    -- Instantiate a cluster config to an instance config for
    -- cluster config sources.
    local iconfig = {}
    local cconfig = {}
    for _, source in ipairs(ctx.sources) do
        local cfg = source.get()
        if source.type == 'cluster' then
            cconfig = cluster_config:merge(cconfig, cfg)
            cfg = cluster_config:instantiate(cconfig, ctx.instance_name)
        end
        iconfig = instance_config:merge(iconfig, cfg)
    end
    ctx.configdata = configdata.new(iconfig, cconfig, ctx.instance_name)
end

local function apply()
    for _, applier in ipairs(ctx.appliers) do
        applier.apply(ctx.configdata)
    end
end

local function startup(instance_name, config_file)
    parse_args(instance_name, config_file)
    initialize()
    collect()
    apply()
end

-- opts:
-- - use_default: boolean
local function get(path, opts)
    if ctx.configdata == nil then
        error('conf.get: no instance config available yet')
    end
    return ctx.configdata:get(path, opts)
end

return {
    startup = startup,
    get = get,
}
