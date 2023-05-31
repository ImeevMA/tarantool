local compat = require('compat')
local tarantool = require('tarantool')
local uuid = require('uuid')
local schema = require('internal.conf.utils.schema')

-- List of annotations:
--
-- * type (string)
--
--   Scalar type.
--
-- * enterprise_edition (boolean)
--
--   Available only in Tarantool Enterprise Edition.
--
-- * default (any)
--
--   Default value.
--
-- * scope ('global', 'group', 'repicaset', 'instance')
--
--   A place of an instance config option in in the cluster config
--   hierarchy.
--
-- * box_cfg (string)
--
--   A name of the corresponding box.cfg() option.
--
-- * box_cfg_nondynamic (boolean)
--
--   `true` if the option can only be set at first box.cfg() call
--   and cannot be changed by a subsequent box.cfg() call.
--
-- * allowed_values
--
--   A list of allowed values.
--
-- * mkdir (boolean)
--
--   Create the given directory before box.cfg().
--
-- * mk_parent_dir (booelan)
--
--   Create a parent directry for the given file before box.cfg().

local function enterprise_edition_validate(_schema, data, w)
    -- OK if we're on Tarantool EE.
    if tarantool.package == 'Tarantool Enterprise' then
        return
    end

    assert(tarantool.package == 'Tarantool')

    -- OK, if the value is null or empty.
    if data == nil then
        return
    end
    if type(data) == 'table' and next(data) == nil then
        return
    end

    w.error('This configuration parameter is available only in Tarantool ' ..
        'Enterprise Edition')
end

local function enterprise_edition_apply_default_if(_schema, _data, _w)
    return tarantool.package == 'Tarantool Enterprise'
end

-- Available only in Tarantool Enterprise Edition.
local function enterprise_edition(schema_node)
    schema_node.enterprise_edition = true
    schema_node.validate = enterprise_edition_validate
    schema_node.apply_default_if = enterprise_edition_apply_default_if
    return schema_node
end

local function validate_uuid_str(_schema, data, w)
    if uuid.fromstr(data) == nil then
        w.error('Unable to parse the value as a UUID: %q', data)
    end
    if data == uuid.NULL:str() then
        w.error('nil UUID is reserved')
    end
end

return schema.new('instance_config', schema.record({
    config = schema.record({
        version = schema.scalar({
            type = 'string',
            -- TODO: Require the field.
            --
            -- We should handle it in the env source: set the
            -- latest config version if unset.
            --[[
            validate = function(_schema, data, w)
                if data == nil then
                    w.error('config.version is mandatory')
                end
                if data ~= CONFIG_VERSION then
                    w.error('config.version should be %s', CONFIG_VERSION)
                end
            end,
            ]]--
        }),
        -- TODO: Not handled anywhere.
        hooks = schema.record({
            pre_cfg = schema.union_of_records(schema.record({
                file = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
            }), schema.record({
                module = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
            })),
            post_cfg = schema.union_of_records(schema.record({
                file = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
            }), schema.record({
                module = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
            })),
        }),
        -- Defaults can't be set there, because the `validate`
        -- annotation expects either no data or data with existing
        -- prefix field. The prefix field has no default. So,
        -- applying defaults to an empty data would make the data
        -- invalid.
        etcd = enterprise_edition(schema.record({
            prefix = schema.scalar({
                type = 'string',
                validate = function(_schema, data, w)
                    if not data:startswith('/') then
                        w.error(('config.etcd.prefix should be a path alike ' ..
                            'value, got %q'):format(data))
                    end
                end,
            }),
            endpoints = schema.array({
                items = schema.scalar({
                    type = 'string',
                }),
            }),
            username = schema.scalar({
                type = 'string',
            }),
            password = schema.scalar({
                type = 'string',
            }),
            http = schema.record({
                request = schema.record({
                    timeout = schema.scalar({
                        type = 'number',
                        -- default = 0.3 is applied right in the
                        -- etcd source. See a comment above
                        -- regarding defaults in config.etcd.
                    }),
                    unix_socket = schema.scalar({
                        type = 'string',
                    }),
                }),
            }),
            ssl = schema.record({
                ssl_key = schema.scalar({
                    type = 'string',
                }),
                ca_path = schema.scalar({
                    type = 'string',
                }),
                ca_file = schema.scalar({
                    type = 'string',
                }),
                verify_peer = schema.scalar({
                    type = 'boolean',
                }),
                verify_host = schema.scalar({
                    type = 'boolean',
                }),
            }),
        }, {
            validate = function(_schema, data, w)
                -- No config.etcd section at all -- OK.
                if data == nil or next(data) == nil then
                    return
                end
                -- There is some data -- the prefix should be there.
                if data.prefix == nil then
                    w.error('No config.etcd.prefix provided')
                end
            end,
        })),
    }),
    process = schema.record({
        strip_core = schema.scalar({
            type = 'boolean',
            box_cfg = 'strip_core',
            default = true,
        }),
        coredump = schema.scalar({
            type = 'boolean',
            box_cfg = 'coredump',
            default = false,
        }),
        background = schema.scalar({
            type = 'boolean',
            box_cfg = 'background',
            default = false,
        }),
        title = schema.scalar({
            type = 'string',
            box_cfg = 'custom_proc_title',
            default = 'tarantool - {{ instance_name }}',
        }),
        username = schema.scalar({
            type = 'string',
            box_cfg = 'username',
            default = box.NULL,
        }),
        work_dir = schema.scalar({
            type = 'string',
            box_cfg = 'work_dir',
            default = box.NULL,
        }),
        pid_file = schema.scalar({
            type = 'string',
            box_cfg = 'pid_file',
            mk_parent_dir = true,
            default = box.NULL,
            -- TODO: There is a proposal to set the following
            -- default here.
            --
            -- default = '/var/run/tarantool/{{ instance_name }}.pid',
            --
            -- But we should handle permission denied for running
            -- as a regular user.
            --
            -- BTW, it seems better to use something like
            -- {{ TARANTOOL_RUNDIR }} instead of hardcoded
            -- /var/run.
        }),
    }),
    console = schema.record({
        enabled = schema.scalar({
            type = 'boolean',
            default = true,
        }),
        socket = schema.scalar({
            type = 'string',
            -- XXX: Use TARANTOOL_RUNDIR.
            default = '/var/run/tarantool/{{ instance_name }}.control',
        }),
    }),
    fiber = schema.record({
        io_collect_interval = schema.scalar({
            type = 'number',
            box_cfg = 'io_collect_interval',
            default = box.NULL,
        }),
        too_long_threshold  = schema.scalar({
            type = 'number',
            box_cfg = 'too_long_threshold',
            default = 0.5,
        }),
        worker_pool_threads = schema.scalar({
            type = 'number',
            box_cfg = 'worker_pool_threads',
            default = 4,
        }),
        slice = schema.record({
            warn = schema.scalar({
                type = 'number',
                default = 0.5,
            }),
            err = schema.scalar({
                type = 'number',
                default = 1,
            }),
        }),
        top = schema.record({
            enabled = schema.scalar({
                type = 'boolean',
                default = false,
            }),
        }),
    }),
    log = schema.record({
        -- The logger destination is handled separately in the
        -- box_cfg applier, so there are no explicit box_cfg and
        -- box_cfg_nondynamic annotations.
        --
        -- The reason is that there is no direct-no-transform
        -- mapping from, say, `log.file` to `box_cfg.log`.
        -- The applier should add the `file:` prefix.
        to = schema.enum({
            'stderr',
            'file',
            'pipe',
            'syslog',
        }, {
            default = 'stderr',
        }),
        file = schema.scalar({
            type = 'string',
            default = '/var/log/tarantool/{{ instance_name }}.log',
        }),
        pipe = schema.scalar({
            type = 'string',
            default = box.NULL,
        }),
        syslog = schema.record({
            identity = schema.scalar({
                type = 'string',
                default = 'tarantool',
            }),
            facility = schema.scalar({
                type = 'string',
                default = 'local7',
            }),
            server = schema.scalar({
                type = 'string',
                -- The logger tries /dev/log and then
                -- /var/run/syslog if no server is provided.
                default = box.NULL,
            }),
        }),
        nonblock = schema.scalar({
            type = 'boolean',
            box_cfg = 'log_nonblock',
            box_cfg_nondynamic = true,
            default = false,
        }),
        level = schema.scalar({
            type = 'number, string',
            box_cfg = 'log_level',
            default = 5,
            allowed_values = {
                0, 'fatal',
                1, 'syserror',
                2, 'error',
                3, 'crit',
                4, 'warn',
                5, 'info',
                6, 'verbose',
                7, 'debug',
            },
        }),
        format = schema.scalar({
            type = 'string',
            box_cfg = 'log_format',
            default = 'plain',
        }),
        -- box.cfg({log_modules = <...>}) replaces the previous
        -- value without any merging.
        --
        -- If a key in this map is removed in the provided
        -- configuration, then it will be removed in the actually
        -- applied configuration.
        --
        -- It is exactly what we need there to make the
        -- configuration independent of previously applied values.
        modules = schema.map({
            key = schema.scalar({
                type = 'string',
            }),
            value = schema.scalar({
                type = 'number, string',
            }),
            box_cfg = 'log_modules',
            -- TODO: This default doesn't work now. It needs
            -- support of non-scalar schema nodes in
            -- <schema object>:map().
            default = box.NULL,
        }),
    }, {
        validate = function(_schema, log, w)
            if log.to == 'pipe' and log.pipe == nil then
                w.error('The pipe logger is set by the log.to parameter but ' ..
                    'the command is not set (log.pipe parameter)')
            end
        end,
    }),
    iproto = schema.record({
        -- XXX: listen/advertise are specific: accept a string of
        -- a particular format, a number (port), a table of a
        -- particular format.
        --
        -- Only a string (without further validation) is accepted
        -- for now.
        listen = schema.scalar({
            type = 'string',
            box_cfg = 'listen',
            default = box.NULL,
        }),
        advertise = schema.scalar({
            type = 'string',
            default = box.NULL,
        }),
        threads = schema.scalar({
            type = 'integer',
            box_cfg = 'iproto_threads',
            default = 1,
        }),
        net_msg_max = schema.scalar({
            type = 'integer',
            box_cfg = 'net_msg_max',
            default = 768,
        }),
        readahead = schema.scalar({
            type = 'integer',
            box_cfg = 'readahead',
            default = 16320,
        }),
    }),
    database = schema.record({
        instance_uuid = schema.scalar({
            type = 'string',
            box_cfg = 'instance_uuid',
            default = box.NULL,
            validate = validate_uuid_str,
        }),
        replicaset_uuid = schema.scalar({
            type = 'string',
            box_cfg = 'replicaset_uuid',
            default = box.NULL,
            validate = validate_uuid_str,
        }),
        hot_standby = schema.scalar({
            type = 'boolean',
            box_cfg = 'hot_standby',
            default = false,
        }),
        -- Reversed and applied to box_cfg.read_only.
        rw = schema.scalar({
            type = 'boolean',
            default = false,
        }),
        txn_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'txn_timeout',
            default = 365 * 100 * 86400,
        }),
        txn_isolation = schema.enum({
            'read-committed',
            'read-confirmed',
            'best-effort',
        }, {
            box_cfg = 'txn_isolation',
            default = 'best-effort',
        }),
        use_mvcc_engine = schema.scalar({
            type = 'boolean',
            box_cfg = 'memtx_use_mvcc_engine',
            default = false,
        }),
    }),
    sql = schema.record({
        cache_size = schema.scalar({
            type = 'integer',
            box_cfg = 'sql_cache_size',
            default = 5 * 1024 * 1024,
        }),
    }),
    memtx = schema.record({
        memory = schema.scalar({
            type = 'integer',
            box_cfg = 'memtx_memory',
            default = 256 * 1024 * 1024,
        }),
        allocator = schema.enum({
            'small',
            'system',
        }, {
            box_cfg = 'memtx_allocator',
            default = 'small',
        }),
        slab_alloc_granularity = schema.scalar({
            type = 'integer',
            box_cfg = 'slab_alloc_granularity',
            default = 8,
        }),
        slab_alloc_factor = schema.scalar({
            type = 'number',
            box_cfg = 'slab_alloc_factor',
            default = 1.05,
        }),
        min_tuple_size = schema.scalar({
            type = 'integer',
            box_cfg = 'memtx_min_tuple_size',
            default = 16,
        }),
        max_tuple_size = schema.scalar({
            type = 'integer',
            box_cfg = 'memtx_max_tuple_size',
            default = 1024 * 1024,
        }),
    }),
    vinyl = schema.record({
        -- TODO: vinyl options.
        dir = schema.scalar({
            type = 'string',
            box_cfg = 'vinyl_dir',
            mkdir = true,
            default = '.',
        }),
        max_tuple_size = schema.scalar({
            type = 'integer',
            box_cfg = 'vinyl_max_tuple_size',
            default = 1024 * 1024,
        }),
    }),
    wal = schema.record({
        dir = schema.scalar({
            type = 'string',
            box_cfg = 'wal_dir',
            mkdir = true,
            default = '.',
        }),
        mode = schema.enum({
            'none',
            'write',
            'fsync',
        }, {
            box_cfg = 'wal_mode',
            default = 'write',
        }),
        max_size = schema.scalar({
            type = 'integer',
            box_cfg = 'wal_max_size',
            default = 256 * 1024 * 1024,
        }),
        dir_rescan_delay = schema.scalar({
            type = 'number',
            box_cfg = 'wal_dir_rescan_delay',
            default = 2,
        }),
        queue_max_size = schema.scalar({
            type = 'integer',
            box_cfg = 'wal_queue_max_size',
            default = 16 * 1024 * 1024,
        }),
        cleanup_delay = schema.scalar({
            type = 'number',
            box_cfg = 'wal_cleanup_delay',
            default = 4 * 3600,
        }),
        -- box.cfg({wal_ext = <...>}) replaces the previous
        -- value without any merging. See explanation why it is
        -- important in the log.modules description.
        ext = enterprise_edition(schema.record({
            old = schema.scalar({
                type = 'boolean',
                -- TODO: This default is applied despite the outer
                -- apply_default_if, because the annotation has no
                -- effect on child schema nodes.
                --
                -- This default is purely informational: lack of the
                -- value doesn't break configuration applying
                -- idempotence.
                -- default = false,
            }),
            new = schema.scalar({
                type = 'boolean',
                -- TODO: See wal.ext.old.
                -- default = false,
            }),
            spaces = schema.map({
                key = schema.scalar({
                    type = 'string',
                }),
                value = schema.record({
                    old = schema.scalar({
                        type = 'boolean',
                        default = false,
                    }),
                    new = schema.scalar({
                        type = 'boolean',
                        default = false,
                    }),
                }),
            }),
        }, {
            box_cfg = 'wal_ext',
            -- TODO: This default doesn't work now. It needs
            -- support of non-scalar schema nodes in
            -- <schema object>:map().
            default = box.NULL,
        })),
    }),
    snapshot = schema.record({
        dir = schema.scalar({
            type = 'string',
            box_cfg = 'memtx_dir',
            mkdir = true,
            default = '.',
        }),
        by = schema.record({
            interval = schema.scalar({
                type = 'number',
                box_cfg = 'checkpoint_interval',
                default = 3600,
            }),
            wal_size = schema.scalar({
                type = 'integer',
                box_cfg = 'checkpoint_wal_threshold',
                default = 1e18,
            }),
        }),
        count = schema.scalar({
            type = 'integer',
            box_cfg = 'checkpoint_count',
            default = 2,
        }),
        snap_io_rate_limit = schema.scalar({
            type = 'number',
            box_cfg = 'snap_io_rate_limit',
            default = box.NULL,
        }),
    }),
    replication = schema.record({
        -- XXX: needs more validation
        peers = schema.array({
            items = schema.scalar({
                type = 'string',
            }),
            box_cfg = 'replication',
            default = box.NULL,
        }),
        anon = schema.scalar({
            type = 'boolean',
            box_cfg = 'replication_anon',
            default = false,
        }),
        threads = schema.scalar({
            type = 'integer',
            box_cfg = 'replication_threads',
            default = 1,
        }),
        timeout = schema.scalar({
            type = 'number',
            box_cfg = 'replication_timeout',
            default = 1,
        }),
        synchro_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'replication_synchro_timeout',
            default = 5,
        }),
        connect_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'replication_connect_timeout',
            default = 30,
        }),
        sync_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'replication_sync_timeout',
            default = compat.box_cfg_replication_sync_timeout.default == 'old'
                and 300 or 0,
        }),
        sync_lag = schema.scalar({
            type = 'number',
            box_cfg = 'replication_sync_lag',
            default = 10,
        }),
        synchro_quorum = schema.scalar({
            type = 'string, number',
            box_cfg = 'replication_synchro_quorum',
            default = 'N / 2 + 1',
        }),
        skip_conflict = schema.scalar({
            type = 'boolean',
            box_cfg = 'replication_skip_conflict',
            default = false,
        }),
        election_mode = schema.enum({
            'off',
            'voter',
            'manual',
            'candidate',
        }, {
            box_cfg = 'election_mode',
            default = 'off',
        }),
        election_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'election_timeout',
            default = 5,
        }),
        election_fencing_mode = schema.enum({
            'off',
            'soft',
            'strict',
        }, {
            box_cfg = 'election_fencing_mode',
            default = 'soft',
        }),
        bootstrap_strategy = schema.enum({
            'auto',
            'config',
            'supervised',
            'legacy',
        }, {
            box_cfg = 'bootstrap_strategy',
            default = 'auto',
        }),
    }),
    -- Unlike other sections, credentials contains the append-only
    -- parameters. It means that deletion of a value from the
    -- config doesn't delete the corresponding user/role/privilege.
    credentials = schema.record({
        roles = schema.map({
            -- Name of the role.
            key = schema.scalar({
                type = 'string',
            }),
            -- Privileges granted to the role.
            value = schema.record({
                privileges = schema.array({
                    items = schema.record({
                        permissions = schema.set({
                            'super',
                            'read',
                            'write',
                            'execute',
                            'create',
                            'alter',
                            'drop',
                            'usage',
                            'session',
                        }),
                        universe = schema.scalar({
                            type = 'boolean',
                        }),
                        spaces = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                        functions = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                        sequences = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                    }),
                }),
                -- The given role has all the privileges from
                -- these underlying roles.
                roles = schema.array({
                    items = schema.scalar({
                        type = 'string',
                    }),
                }),
            }),
        }),
        users = schema.map({
            -- Name of the user.
            key = schema.scalar({
                type = 'string',
            }),
            -- Parameters of the user.
            value = schema.record({
                password = schema.union_of_records(
                    schema.record({
                        plain = schema.scalar({
                            type = 'string',
                        }),
                    }),
                    schema.record({
                        sha1 = schema.scalar({
                            type = 'string',
                        }),
                    }),
                    schema.record({
                        sha256 = schema.scalar({
                            type = 'string',
                        }),
                    })
                ),
                privileges = schema.array({
                    items = schema.record({
                        permissions = schema.set({
                            'super',
                            'read',
                            'write',
                            'execute',
                            'create',
                            'alter',
                            'drop',
                            'usage',
                            'session',
                        }),
                        universe = schema.scalar({
                            type = 'boolean',
                        }),
                        spaces = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                        functions = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                        sequences = schema.array({
                            items = schema.scalar({
                                type = 'string',
                            }),
                        }),
                    }),
                }),
                -- The given user has all the privileges from
                -- these underlying roles.
                roles = schema.array({
                    items = schema.scalar({
                        type = 'string',
                    }),
                }),
            }),
        }),
    }),
    -- TODO: audit
    -- TODO: security
    -- TODO: feedback
    -- TODO: flightrec
    -- TODO: metrics
    -- TODO: app
}))
