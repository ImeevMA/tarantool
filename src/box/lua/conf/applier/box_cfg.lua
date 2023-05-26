local log = require('conf.utils.log')

local function peer_uris(configdata)
    local peers = configdata:peers()
    if #peers <= 1 then
        return nil
    end

    local uris = {}
    for _, peer_name in ipairs(peers) do
        local iproto = configdata:get('iproto', {peer = peer_name}) or {}
        local uri = iproto.advertise or iproto.listen
        if uri == nil then
            -- XXX: Raise an error in the case?
            log.warn('box_cfg.apply: neither iproto.advertise nor ' ..
                'iproto.listen provided for peer %q; do not construct ' ..
                'box.cfg.replication', peer_name)
            return nil
        end
        table.insert(uris, uri)
    end
    return uris
end

local function log_to(configdata)
    local to = configdata:get('log.to', {use_default = true})
    if to == nil then
        return box.NULL
    elseif to.file ~= nil then
        return ('file:%s'):format(to.file)
    elseif to.pipe ~= nil then
        return ('pipe:%s'):format(to.pipe)
    elseif to.syslog ~= nil and to.syslog.enabled then
        -- TODO: It would be nice to move the default values into
        -- the instance config schema, but the `default`
        -- annotation means a kind of 'absolute default' -- it is
        -- added to the configdata unconditionally if no data is
        -- provided. So if a user set...
        --
        -- {
        --     log = {
        --         to = {
        --             file = '<...>',
        --         },
        --     },
        -- }
        --
        -- ...we'll get...
        --
        -- {
        --     log = {
        --         to = {
        --             file = '<...>',
        --             syslog = {<...>},
        --         },
        --     },
        -- }
        --
        -- ...with default values in the `syslog` key. It is hard
        -- to determine, whether to configure logger as file or as
        -- syslog one by this configuration.
        --
        -- It can be solved by some sort of 'annotational
        -- defaults', which just present in the schema and
        -- accessible for configuration appliers.
        --
        -- Another variant is to implement a conditional default,
        -- which is added to the config data only if certain
        -- conditions are met.
        --
        -- Another problem is that the `server` default is
        -- platform-dependent.
        local res = ('syslog:identity=%s,facility=%s'):format(
            to.syslog.identity or 'tarantool',
            to.syslog.facility or 'local7')
        -- TODO: Syslog's URI format is different from tarantool's
        -- one for Unix domain sockets: `unix:/path/to/socket` vs
        -- `unix/:/path/to/socket`. We expect syslog's format
        -- here, but maybe it worth to accept our own one (or even
        -- just path) and transform under hood.
        if to.syslog.server ~= nil then
            res = res .. (',server=%s'):format(to.server)
        end
        return res
    else
        -- If no file/pipe/syslog is set, the `log.to` object
        -- is allowed to not present or to be null or empty. All
        -- the variants mean 'use internal defaults'.
        assert(next(to) == nil)
    end
end

local function apply(configdata)
    local box_cfg = configdata:filter(function(w)
        return w.schema.box_cfg ~= nil
    end, {use_default = true}):map(function(w)
        return w.schema.box_cfg, w.data
    end):tomap()

    -- Construct box_cfg.replication.
    if box_cfg.replication == nil then
        box_cfg.replication = peer_uris(configdata)
    end

    -- Construct logger destination (box_cfg.log) and log modules.
    --
    -- `log.nonblock`, `log.level`, `log.format` options are
    -- marked with the `box_cfg` annotations and so they're
    -- already added to `box_cfg`.
    --
    -- TODO: Forbid at non-first box.cfg().
    box_cfg.log = log_to(configdata)
    box_cfg.log_modules = configdata:get('log.modules', {use_default = true})

    box_cfg.read_only = not configdata:get('database.rw', {use_default = true})

    log.debug('box_cfg.apply: %s', box_cfg)

    box.cfg(box_cfg)

    -- Add instance, replicaset and group (cluster) names.
    --
    -- The names can't be passed to the first box.cfg() call,
    -- because it would break a scenario with upgrading from a
    -- snapshot without those names.
    --
    -- Second box.cfg() call, however, accept the names if there
    -- were no ones before.
    --
    -- At the same time, first box.cfg() allows to omit the names
    -- and start from a snapshot with the names.
    --
    -- So, all the scerarios look working, when we omit the
    -- options at first box.cfg() call, but pass them to the
    -- second call:
    --
    -- * Upgrade from a snapshot without names.
    -- * Start from a snapshot with the names.
    --
    -- However, there are downsides:
    --
    -- * If one of the names doesn't correspond to one written to
    --   a snapshot (a real mistake occurs), the user will get the
    --   mismatch error only after full (and possibly incorrect)
    --   startup, including synchronization with replication
    --   peers.
    -- * There is no check on a read-only instance.
    if box.info.ro then
        log.verbose('box_cfg.apply (second phase): skip ' ..
            'cluster/replicaset/instance names applying: the instance is in ' ..
            'the read-only mode')
    else
        local names = configdata:names()
        local box_cfg_2 = {
            cluster_name = names.group_name,
            replicaset_name = names.replicaset_name,
            instance_name = names.instance_name,
        }
        log.debug('box_cfg.apply (second phase): %s', box_cfg_2)
        box.cfg(box_cfg_2)
    end
end

return {
    name = 'box_cfg',
    apply = apply,
}
