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

g.test_log = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
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
            format = 'six',
            modules = {
                seven = 'debug',
            },
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.log, instance_config.schema.fields.log)

    iconfig = {
        config = {
            version = '3.0.0',
        },
        log = {
            level = 5,
        }
    }
    ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)

    iconfig = {
        config = {
            version = '3.0.0',
        },
        log = {
            to = 'pipe',
        }
    }
    local err = '[instance_config] log: The pipe logger is set by the log.to '..
                'parameter but the command is not set (log.pipe parameter)'
    t.assert_error_msg_equals(err, function()
        instance_config:validate(iconfig)
    end)
end

g.test_iproto = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        iproto = {
            listen = 'one',
            advertise = 'two',
            threads = 1,
            net_msg_max = 1,
            readahead = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.iproto, instance_config.schema.fields.iproto)
end

g.test_database = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        database = {
            instance_uuid = '11111111-1111-1111-1111-111111111111',
            replicaset_uuid = '11111111-1111-1111-1111-111111111111',
            hot_standby = true,
            rw = true,
            txn_timeout = 1,
            txn_isolation = 'best-effort',
            use_mvcc_engine = true,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.database, instance_config.schema.fields.database)

    iconfig = {
        config = {
            version = '3.0.0',
        },
        database = {
            instance_uuid = '1',
        }
    }
    local err = '[instance_config] database.instance_uuid: Unable to parse '..
                'the value as a UUID: "1"'
    t.assert_error_msg_content_equals(err, function()
        instance_config:validate(iconfig)
    end)

    iconfig = {
        config = {
            version = '3.0.0',
        },
        database = {
            replicaset_uuid = '1',
        }
    }
    err = '[instance_config] database.replicaset_uuid: Unable to parse the '..
          'value as a UUID: "1"'
    t.assert_error_msg_content_equals(err, function()
        instance_config:validate(iconfig)
    end)
end

g.test_sql = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        sql = {
            cache_size = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.sql, instance_config.schema.fields.sql)
end

g.test_memtx = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        memtx = {
            memory = 1,
            allocator = 'small',
            slab_alloc_granularity = 1,
            slab_alloc_factor = 1,
            min_tuple_size = 1,
            max_tuple_size = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.memtx, instance_config.schema.fields.memtx)
end

g.test_vinyl = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        vinyl = {
            dir = 'one',
            max_tuple_size = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.vinyl, instance_config.schema.fields.vinyl)
end

g.test_wal = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        wal = {
            dir = 'one',
            mode = 'none',
            max_size = 1,
            dir_rescan_delay = 1,
            queue_max_size = 1,
            cleanup_delay = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.wal, instance_config.schema.fields.wal)
end

g.test_snapshot = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        snapshot = {
            dir = 'one',
            by = {
                interval = 1,
                wal_size = 1,
                rows_per_wal = 1,
            },
            count = 1,
            snap_io_rate_limit = 1,
        }
    }
    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.snapshot, instance_config.schema.fields.snapshot)
end

g.test_replication = function()
    local iconfig = {
        config = {
            version = '3.0.0',
        },
        replication = {
            peers = {'one', 'two'},
            anon = true,
            threads = 1,
            timeout = 1,
            synchro_timeout = 1,
            connect_timeout = 1,
            sync_timeout = 1,
            sync_lag = 1,
            synchro_quorum = 1,
            skip_conflict = true,
            election_mode = 'off',
            election_timeout = 1,
            election_fencing_mode = 'off',
            bootstrap_strategy = 'auto',
        }
    }

    local ok = pcall(instance_config.validate, instance_config, iconfig)
    t.assert(ok)
    validate_fields(iconfig.replication,
                    instance_config.schema.fields.replication)
end
