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
    -- Collected config values.
    configdata = nil,
    -- TODO: Track applied configdata as well.
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
    -- The sources are synchronized in the order of registration:
    -- env, file, etcd (the latter is present in Tarantool EE).
    --
    -- The configuration values from the first source has highest
    -- priority. The menthal rule here is the following: values
    -- closer to the process are preferred: env first, then file,
    -- then etcd (if available).
    register_source(require('conf.source.env'))

    if ctx.config_file ~= nil then
        register_source(require('conf.source.file'))
    end

    register_applier(require('conf.applier.mkdir'))
    register_applier(require('conf.applier.box_cfg'))
    register_applier(require('conf.applier.credentials'))
    register_applier(require('conf.applier.console'))
    register_applier(require('conf.applier.fiber'))

    -- Tarantool Enterprise Edition has its own additions
    -- for this module.
    local ok, extras = pcall(require, 'conf.extras')
    if ok then
        extras.initialize(ctx)
    end
end

local function collect()
    local iconfig = {}
    local cconfig = {}

    for _, source in ipairs(ctx.sources) do
        -- Gather config values.
        --
        -- The configdata object is not constructed yet, so we
        -- pass currently collected instance config as the second
        -- argument. The 'config' section of the config may
        -- contain a configuration needed for a source.
        source.sync(ctx, iconfig)

        -- Validate configurations gathered from the sources.
        if source.type == 'instance' then
            instance_config:validate(source.get())
        elseif source.type == 'cluster' then
            cluster_config:validate(source.get())
        else
            assert(false)
        end

        -- TODO: We need specific validators at least for the
        -- instance config. For example, there should be a
        -- validator, which forbids Tarantool EE options in
        -- Tarantool CE.

        -- Merge configurations from the sources.
        --
        -- Instantiate a cluster config to an instance config for
        -- cluster config sources.
        --
        -- The configuration values from a first source has highest
        -- priority. We should keep already gathered values in the
        -- accumulator and fill only missed ones from next sources.
        --
        -- :merge() prefers values from the second argument, so the
        -- accumulator is passed as the second.
        local source_iconfig
        if source.type == 'cluster' then
            local source_cconfig = source.get()
            cconfig = cluster_config:merge(source_cconfig, cconfig)
            source_iconfig = cluster_config:instantiate(cconfig, ctx.instance_name)
        elseif source.type == 'instance' then
            source_iconfig = source.get()
        else
            assert(false)
        end
        iconfig = instance_config:merge(source_iconfig, iconfig)
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
