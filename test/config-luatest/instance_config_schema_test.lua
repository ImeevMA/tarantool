local t = require('luatest')
local instance_config = require('internal.config.instance_config')

local g = t.group()

-- Check that all record element names can be found in the table and vice versa.
local function validate_fields(config, record)
    local config_fields = {}
    if type(config) == 'table' then
        for k in pairs(config) do
            table.insert(config_fields, k)
        end
    end

    local record_fields = {}
    for k, v in pairs(record.fields) do
        if v.type == 'record' then
            validate_fields(config[k], v)
        end
        table.insert(record_fields, k)
    end

    t.assert_equals(config_fields, record_fields)
end

g.test_general = function()
    t.assert_equals(instance_config.name, 'instance_config')
end

g.test_config = function()
    local err = '[instance_config] config.version is mandatory'
    t.assert_error_msg_equals(err, function() instance_config:validate({}) end)

    local iconfig = {
        config = {
            version = '3.0.0',
            reload = 'auto',
        },
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.config, instance_config.schema.fields.config)
end

g.test_process = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        process = {
            strip_core = true,
            coredump = true,
            background = true,
            title = 'one',
            username = 'two',
            work_dir = 'three',
            pid_file = 'four',
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.process, instance_config.schema.fields.process)
end

g.test_console = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        console = {
            enabled = true,
            socket = 'one',
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.console, instance_config.schema.fields.console)
end

g.test_fiber = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        fiber = {
            io_collect_interval = 1,
            too_long_threshold = 1,
            worker_pool_threads = 1,
            slice = {
                warn = 1,
                err = 1,
            },
            top = {
                enabled = true,
            },
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.fiber, instance_config.schema.fields.fiber)
end
