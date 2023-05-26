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

-- Remove indent from a text.
--
-- Similar to Python's textwrap.dedent().
--
-- It strips all newlines from beginning and all whitespace
-- characters from the end for convenience use with multiline
-- string literals ([[ <...> ]]).
local function dedent(s)
    local lines = s:lstrip('\n'):rstrip():split('\n')

    local indent = math.huge
    for _, line in ipairs(lines) do
        if #line ~= 0 then
            indent = math.min(indent, #line:match('^ *'))
        end
    end

    local res = {}
    for _, line in ipairs(lines) do
        table.insert(res, line:sub(indent + 1))
    end
    return table.concat(res, '\n')
end

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

    -- For error reporting.
    local source_info = {}

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

        -- If a source returns an empty table, mark it as ones
        -- that provide no data.
        local has_data = next(source.get()) ~= nil
        table.insert(source_info, ('* %q [type: %s]%s'):format(source.name,
            source.type, has_data and '' or ' (no data)'))
    end

    if next(cconfig) == nil then
        error(dedent([[
            Startup failure.

            No cluster config received from the given configuration sources.

            %s

            The %q instance cannot find itself in the group/replicaset/instance
            topology and it is unknown, whether it should join a replicaset or
            create its own database.

            Recipes:

            * Use --config <file> command line option.
            * Use TT_CONFIG_ETCD_* environment variables (available on Tarantool
              Enterprise Edition).
        ]]):format(table.concat(source_info, '\n'), ctx.instance_name), 0)
    end

    if cluster_config:find_instance(cconfig, ctx.instance_name) == nil then
        error(dedent([[
            Startup failure.

            Unable to find instance %q in the group/replicaset/instance
            topology provided by the given cluster configuration sources.

            %s

            It is unknown, whether the instance should join a replicaset or
            create its own database.

            Minimal cluster config:

            groups:
              group-001:
                replicasets:
                  replicaset-001:
                    instances:
                      instance-001:
                        database:
                          rw: true
        ]]):format(ctx.instance_name, table.concat(source_info, '\n')), 0)
    end

    ctx.configdata = configdata.new(iconfig, cconfig, ctx.instance_name)
end

local function apply()
    for _, applier in ipairs(ctx.appliers) do
        applier.apply(ctx.configdata)
    end
end

local function startup(instance_name, config_file)
    ctx.instance_name = instance_name
    ctx.config_file = config_file

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
