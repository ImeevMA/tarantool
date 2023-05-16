local schema = require('conf.utils.schema')

-- List of annotations:
--
-- * type (string)
--
--   Scalar type.
--
-- * required (boolean)
--
--   TODO: Not implemented yet.
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

-- Available only in Tarantool Enterprise Edition.
local function enterprise_edition(schema_node)
    return schema.annotate(schema_node, {
        enterprise_edition = true,
    })
end

return schema.new('instance_config', schema.record({
    config = schema.record({
        -- TODO: Not handled anywhere.
        version = schema.scalar({
            type = 'string',
            -- TODO: Support required fields in the schema.
            required = true,
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
        -- Supported in Tarantool Entreprise Edition.
        --
        -- Ignored in Tarantool Community Edition.
        --
        -- TODO: Is it better to give an error if one of those
        -- options arrive to CE?
        etcd = enterprise_edition(schema.record({
            prefix = schema.scalar({
                type = 'string',
                default = nil,
            }),
            endpoints = schema.scalar({
                type = '[string]',
                default = nil,
            }),
            -- TODO: Wait for support in the etcd-client.
            --
            -- TODO: Name it `discovery_interval` or similarly to
            -- set a period (zero would mean disabling the
            -- option)? The official Go etcd client has the
            -- `AutoSyncInterval` option.
            --
            -- See https://github.com/tarantool/etcd-client/issues/35.
            --[[
            discover_endpoints = schema.scalar({
                type = 'boolean',
            }),
            ]]--
            username = schema.scalar({
                type = 'string',
                default = nil,
            }),
            password = schema.scalar({
                type = 'string',
                default = nil,
            }),
            http = schema.record({
                request = schema.record({
                    timeout = schema.scalar({
                        type = 'number',
                        default = 0.3,
                    }),
                    unix_socket = schema.scalar({
                        type = 'string',
                        default = nil,
                    }),
                }),
            }),
            ssl = schema.record({
                ssl_key = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
                ca_path = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
                ca_file = schema.scalar({
                    type = 'string',
                    default = nil,
                }),
                verify_peer = schema.scalar({
                    type = 'boolean',
                    default = nil,
                }),
                verify_host = schema.scalar({
                    type = 'boolean',
                    default = nil,
                }),
            }),
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
        -- TODO: Accept path, forbid other strings.
        socket = schema.scalar({
            type = 'string',
            -- XXX: Use TARANTOOL_RUNDIR.
            default = '/var/run/tarantool/{{ instance_name }}.socket',
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
        -- mapping from, say, `log.to.file` to `box_cfg.log`.
        -- The applier should add the `file:` prefix.
        to = schema.union_of_records(schema.record({
            file = schema.scalar({
                type = 'string',
            }),
        }), schema.record({
            pipe = schema.scalar({
                type = 'string',
            }),
        }), schema.record({
            syslog = schema.record({
                enabled = schema.scalar({
                    type = 'boolean',
                }),
                identity = schema.scalar({
                    type = 'string',
                }),
                facility = schema.scalar({
                    type = 'string',
                }),
                server = schema.scalar({
                    type = 'string',
                }),
            }),
        })),
        nonblock = schema.scalar({
            type = 'boolean',
            box_cfg = 'log_nonblock',
            box_cfg_nondynamic = true,
            default = false,
        }),
        level = schema.scalar({
            type = 'number, string',
            box_cfg = 'log_level',
            default = 5, -- info
        }),
        format = schema.scalar({
            type = 'string',
            box_cfg = 'log_format',
            default = 'plain',
        }),
        modules = schema.map({
            key = schema.scalar({
                type = 'string',
            }),
            value = schema.scalar({
                type = 'number, string',
            }),
            -- TODO: Add {} as default. It may need some work in
            -- schema.lua, because defaults are assumed on scalars
            -- at the moment.
        }),
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
        -- XXX: needs more validation
        instance_uuid = schema.scalar({
            type = 'string',
            box_cfg = 'instance_uuid',
            default = box.NULL,
        }),
        -- XXX: needs more validation
        replicaset_uuid = schema.scalar({
            type = 'string',
            box_cfg = 'replicaset_uuid',
            default = box.NULL,
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
        -- XXX: needs more validation, it is enum in fact
        txn_isolation = schema.scalar({
            type = 'string',
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
        -- XXX: needs more validation, it is enum in fact
        allocator = schema.scalar({
            type = 'string',
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
            default = '.',
        }),
        -- XXX: needs more validation, it is enum in fact
        mode = schema.scalar({
            type = 'string',
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
        -- This option is passed to box_cfg.wal_ext by the box_cfg
        -- configuration applier. There is no box_cfg annotation,
        -- because it is supposed to be used in scalars and some
        -- schema.lua changes may be needed to support it for
        -- records.
        ext = enterprise_edition(schema.record({
            old = schema.scalar({
                type = 'boolean',
                default = false,
            }),
            new = schema.scalar({
                type = 'boolean',
                default = false,
            }),
            -- TODO: Add {spaces = box.NULL} default to make
            -- reconfiguration with a config without the spaces
            -- actually remove the spaces from the configuration.
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
        peers = schema.scalar({
            type = '[string]',
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
        -- XXX: The default depends on the compat option.
        sync_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'replication_sync_timeout',
            default = 300,
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
        -- XXX: needs more validation, it is enum in fact
        election_mode = schema.scalar({
            type = 'string',
            box_cfg = 'election_mode',
            default = 'off',
        }),
        election_timeout = schema.scalar({
            type = 'number',
            box_cfg = 'election_timeout',
            default = 5,
        }),
        -- XXX: needs more validation, it is enum in fact
        election_fencing_mode = schema.scalar({
            type = 'string',
            box_cfg = 'election_fencing_mode',
            default = 'soft',
        }),
        -- XXX: needs more validation, it is enum in fact
        bootstrap_strategy = schema.scalar({
            type = 'string',
            box_cfg = 'bootstrap_strategy',
            default = 'auto',
        }),
    }),
    credentials = schema.record({
        -- XXX: needs more validation, it is enum in fact
        mode = schema.scalar({
            type = 'string',
            values = {'create', 'sync'}
        }),
        roles = schema.map({
            -- Rolename
            key = schema.scalar({
                type = 'string'
            }),
            -- Grants
            -- XXX: should be an array of all grants
            -- (privilege + entities).
            value = schema.record({
                -- XXX: actually a set of enums
                -- TODO: annotate unique array (set alike)
                privileges = schema.scalar({
                    type = '[string]',
                    -- TODO: support the annotation
                    values = {'super', 'read', 'write', 'execute', 'create',
                              'alter', 'drop', 'usage', 'session'}
                }),
                universe = schema.scalar({
                    type = 'boolean',
                }),
                spaces = schema.scalar({
                    type = '[string]',
                }),
                functions = schema.scalar({
                    type = '[string]',
                }),
                sequences = schema.scalar({
                    type = '[string]',
                }),
                roles = schema.scalar({
                    type = '[string]',
                }),
            }),
        }),
        users = schema.map({
            key = schema.scalar({
                type = 'string'
            }),
            value = schema.record({
                passwd = schema.record({
                    plain = schema.scalar({
                        type = 'string'
                    }),
                    sha1 = schema.scalar({
                        type = 'string'
                    }),
                    sha256 = schema.scalar({
                        type = 'string'
                    })
                }),
                -- XXX: actually roles, not grants.
                grant = schema.scalar({
                    type = '[string]',
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
