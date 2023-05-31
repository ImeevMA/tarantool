local instance_config = require('internal.conf.instance_config')
local cluster_config = require('internal.conf.cluster_config')
local configdata = require('internal.conf.configdata')

-- {{{ Helpers

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

-- Extract all fields from a table except ones that start from
-- the underscore.
--
-- Useful for __serialize.
local function filter_out_private_fields(t)
    local res = {}
    for k, v in pairs(t) do
        if not k:startswith('_') then
            res[k] = v
        end
    end
    return res
end

-- }}} Helpers

local methods = {}
local mt = {
    __index = methods,
    __serialize = filter_out_private_fields,
}

local function selfcheck(self, method_name)
    if type(self) ~= 'table' or getmetatable(self) ~= mt then
        local fmt_str = 'Use conf:%s(<...>) instead of conf.%s(<...>)'
        error(fmt_str:format(method_name, method_name), 2)
    end
end

function methods._register_source(self, source)
    assert(type(source) == 'table')
    if source.type ~= 'instance' and source.type ~= 'cluster' then
        error("[source %q] source.type must be 'instance' or 'cluster'",
            tostring(source.type))
    end
    assert(source.sync ~= nil)
    assert(source.get ~= nil)
    table.insert(self._sources, source)
end

function methods._register_applier(self, applier)
    table.insert(self._appliers, applier)
end

function methods._initialize(self)
    -- The sources are synchronized in the order of registration:
    -- env, file, etcd (the latter is present in Tarantool EE).
    --
    -- The configuration values from the first source has highest
    -- priority. The menthal rule here is the following: values
    -- closer to the process are preferred: env first, then file,
    -- then etcd (if available).
    self:_register_source(require('internal.conf.source.env'))

    if self._config_file ~= nil then
        self:_register_source(require('internal.conf.source.file'))
    end

    self:_register_applier(require('internal.conf.applier.mkdir'))
    self:_register_applier(require('internal.conf.applier.box_cfg'))
    self:_register_applier(require('internal.conf.applier.credentials'))
    self:_register_applier(require('internal.conf.applier.console'))
    self:_register_applier(require('internal.conf.applier.fiber'))

    -- Tarantool Enterprise Edition has its own additions
    -- for this module.
    local ok, extras = pcall(require, 'internal.conf.extras')
    if ok then
        extras.initialize(self)
    end
end

function methods._collect(self, opts)
    local opts = opts or {}
    local skip_sync = opts.skip_sync

    local iconfig = {}
    local cconfig = {}

    -- For error reporting.
    local source_info = {}

    for _, source in ipairs(self._sources) do
        -- Gather config values.
        --
        -- The configdata object is not constructed yet, so we
        -- pass currently collected instance config as the second
        -- argument. The 'config' section of the config may
        -- contain a configuration needed for a source.
        if not skip_sync then
            source.sync(self, iconfig)
        end

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
            source_iconfig = cluster_config:instantiate(cconfig,
                self._instance_name)
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
        ]]):format(table.concat(source_info, '\n'), self._instance_name), 0)
    end

    if cluster_config:find_instance(cconfig, self._instance_name) == nil then
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
        ]]):format(self._instance_name, table.concat(source_info, '\n')), 0)
    end

    self._configdata = configdata.new(iconfig, cconfig, self._instance_name)
end

function methods._apply(self)
    for _, applier in ipairs(self._appliers) do
        applier.apply(self._configdata)
    end

    self._configdata_applied = self._configdata

    local ok, extras = pcall(require, 'internal.conf.extras')
    if ok then
        extras.post_apply(self)
    end
end

function methods._startup(self, instance_name, config_file)
    self._instance_name = instance_name
    self._config_file = config_file

    self:_initialize()
    self:_collect({skip_sync = false})
    self:_apply()
end

-- opts:
-- - use_default: boolean
function methods.get(self, path, opts)
    selfcheck(self, 'get')
    if self._configdata_applied == nil then
        error('conf.get: no instance config available yet')
    end
    return self._configdata_applied:get(path, opts)
end

-- The object is a singleton. The constructor should be called
-- only once.
local function new()
    return setmetatable({
        _sources = {},
        _appliers = {},
        -- There are values the module need to hold, which are not
        -- part of the configuration. They're stored here.
        _instance_name = nil,
        _config_file = nil,
        -- Collected config values.
        _configdata = nil,
        -- Track applied config values as well.
        _configdata_applied = nil,
        -- Track situations when something is going wrong.
        _alerts = {},
    }, mt)
end

return new()
