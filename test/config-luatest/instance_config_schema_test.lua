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

    t.assert_items_equals(config_fields, record_fields)
end

g.test_general = function()
    t.assert_equals(instance_config.name, 'instance_config')
end

g.test_config = function()
    local iconfig = {
        config = {
            version = 'dev',
            reload = 'auto',
        },
    }
    instance_config:validate(iconfig)
    validate_fields(iconfig.config, instance_config.schema.fields.config)

    iconfig = {
        config = {
            version = '0.0.0',
            reload = 'auto',
        },
    }
    local err = '[instance_config] config.version: Got 0.0.0, but only the '..
                'following values are allowed: dev'
    t.assert_error_msg_equals(err, function()
        instance_config:validate(iconfig)
    end)

    iconfig = {
        config = {
            reload = 'auto',
        },
    }
    t.assert_equals(iconfig.config, instance_config:apply_default().config)
end

g.test_process = function()
    local iconfig = {
        process = {
            strip_core = true,
            coredump = true,
            background = true,
            title = 'one',
            username = 'two',
            work_dir = 'three',
            pid_file = 'four',
        },
    }
    instance_config:validate(iconfig)
    validate_fields(iconfig.process, instance_config.schema.fields.process)

    iconfig = {
        process = {
            strip_core = true,
            coredump = false,
            background = false,
            title = 'tarantool - {{ instance_name }}',
            username = box.NULL,
            work_dir = box.NULL,
            pid_file = '{{ instance_name }}.pid',
        },
    }
    t.assert_equals(iconfig.process, instance_config:apply_default().process)
end

g.test_console = function()
    local iconfig = {
        console = {
            enabled = true,
            socket = 'one',
        },
    }
    instance_config:validate(iconfig)
    validate_fields(iconfig.console, instance_config.schema.fields.console)

    iconfig = {
        console = {
            enabled = true,
            socket = '{{ instance_name }}.control',
        },
    }
    t.assert_equals(iconfig.console, instance_config:apply_default().console)
end

g.test_fiber = function()
    local iconfig = {
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
        },
    }
    instance_config:validate(iconfig)
    validate_fields(iconfig.fiber, instance_config.schema.fields.fiber)

    iconfig = {
        fiber = {
            io_collect_interval = box.NULL,
            too_long_threshold = 0.5,
            worker_pool_threads = 4,
            slice = {
                err = 1,
                warn = 0.5,
            },
            top = {
                enabled = false,
            },
        },
    }
    t.assert_equals(iconfig.fiber, instance_config:apply_default().fiber)
end

g.test_log = function()
    local iconfig = {
        log = {
            to = 'stderr',
            file = 'one',
            pipe = 'two',
            syslog = {
                identity = 'three',
                facility = 'four',
                server = 'five',
            },
            nonblock = true,
            level = 'debug',
            format = 'json',
            modules = {
                seven = 'debug',
            },
        },
    }
    instance_config:validate(iconfig)
    validate_fields(iconfig.log, instance_config.schema.fields.log)

    iconfig = {
        log = {
            level = 5,
        },
    }
    instance_config:validate(iconfig)

    iconfig = {
        log = {
            to = 'pipe',
        },
    }
    local err = '[instance_config] log: The pipe logger is set by the log.to '..
                'parameter but the command is not set (log.pipe parameter)'
    t.assert_error_msg_equals(err, function()
        instance_config:validate(iconfig)
    end)

    iconfig = {
        log = {
            to = 'stderr',
            file = '{{ instance_name }}.log',
            pipe = box.NULL,
            syslog = {
                identity = 'tarantool',
                facility = 'local7',
                server = box.NULL,
            },
            nonblock = false,
            level = 5,
            format = 'plain',
        },
    }
    t.assert_equals(iconfig.log, instance_config:apply_default().log)
end
