local fiber = require('fiber')
local clock = require('clock')
local config = require('config')
local checks = require('checks')
local netbox = require('net.box')

local WATCHER_DELAY = 0.1
local WATCHER_TIMEOUT = 10

local connections = {}
local statuses = {}

local function is_connection_valid(conn, opts)
    if conn == nil or conn.state == 'error' or conn.state == 'closed' then
        return false
    end
    assert(type(opts) == 'table')
    local conn_opts = conn.opts or {}
    if opts.fetch_schema ~= false and conn_opts.fetch_schema == false then
        return false
    end
    return true
end

local function connect(instance_name, opts)
    checks('string', {
        connect_timeout = '?number',
        wait_connected = '?boolean',
        fetch_schema = '?boolean',
    })
    opts = opts or {}

    local conn = connections[instance_name]
    if not is_connection_valid(conn, opts) then
        local uri = config:instance_uri('peer', {instance = instance_name})
        if uri == nil then
            local err = 'No suitable URI provided for instance %q'
            error(err:format(instance_name), 0)
        end

        local conn_opts = {
            connect_timeout = opts.connect_timeout,
            wait_connected = false,
            fetch_schema = opts.fetch_schema,
        }
        local ok, res = pcall(netbox.connect, uri, conn_opts)
        if not ok then
            local msg = 'Unable to connect to instance %q: %s'
            error(msg:format(instance_name, res.message), 0)
        end
        conn = res
        connections[instance_name] = conn
        local function watch_status(key, value)
            statuses[instance_name] = statuses[instance_name] or {}
            statuses[instance_name].mode = value.is_ro and 'ro' or 'rw'
        end
        conn:watch('box.status', watch_status)
    end

    -- If opts.wait_connected is not false we wait until the connection is
    -- established or an error occurs (including a timeout error).
    if opts.wait_connected ~= false and conn:wait_connected() == false then
        local msg = 'Unable to connect to instance %q: connection timeout'
        error(msg:format(instance_name), 0)
    end
    return conn
end

local function connect_to_candidates(candidates)
    local delay = WATCHER_DELAY
    local conn_opts = {connect_timeout = WATCHER_TIMEOUT}
    for _, instance_name in pairs(candidates) do
        local time_connect_end = clock.monotonic() + WATCHER_TIMEOUT
        local ok, conn = pcall(connect, instance_name, conn_opts)
        if ok and conn.state == 'active' then
            local status = statuses[instance_name]
            while status == nil do
                if clock.monotonic() > time_connect_end then
                    conn:close()
                    statuses[instance_name] = nil
                    break
                end
                fiber.sleep(delay)
                status = statuses[instance_name]
            end
        else
            statuses[instance_name] = nil
        end
    end
end

local function is_roles_match(expected_roles, present_roles)
    if expected_roles == nil or next(expected_roles) == nil then
        return true
    end
    if present_roles == nil or next(present_roles) == nil then
        return false
    end

    local roles = {}
    for _, present_role_name in pairs(present_roles) do
        roles[present_role_name] = true
    end
    for _, expected_role_name in pairs(expected_roles) do
        if roles[expected_role_name] == nil then
            return false
        end
    end
    return true
end

local function is_labels_match(expected_labels, present_labels)
    if expected_labels == nil or next(expected_labels) == nil then
        return true
    end
    if present_labels == nil or next(present_labels) == nil then
        return false
    end

    for label, value in pairs(expected_labels) do
        if present_labels[label] ~= value then
            return false
        end
    end
    return true
end

local function is_candidate_match_static(instance_name, opts)
    assert(opts ~= nil and type(opts) == 'table')
    local get_opts = {instance = instance_name}
    return is_roles_match(opts.roles, config:get('roles', get_opts)) and
           is_labels_match(opts.labels, config:get('labels', get_opts))
end

local function is_mode_match(mode, instance_name)
    if mode == nil then
        return true
    end
    if statuses[instance_name] == nil then
        return false
    end
    return statuses[instance_name].mode == mode
end

local function is_candidate_match_dynamic(instance_name, opts)
    assert(opts ~= nil and type(opts) == 'table')
    return is_mode_match(opts.mode, instance_name)
end

local function filter(opts)
    checks({
        labels = '?table',
        roles = '?table',
        mode = '?string',
    })
    opts = opts or {}
    if opts.mode ~= nil and opts.mode ~= 'ro' and opts.mode ~= 'rw' then
        local msg = 'Expected nil, "ro" or "rw", got "%s"'
        error(msg:format(opts.mode), 0)
    end
    local static_opts = {
        labels = opts.labels,
        roles = opts.roles,
    }
    local dynamic_opts = {
        mode = opts.mode,
    }

    -- First, select candidates using the information from the config.
    local static_candidates = {}
    for instance_name in pairs(config:instances()) do
        if is_candidate_match_static(instance_name, static_opts) then
            table.insert(static_candidates, instance_name)
        end
    end
    -- Return if retrieving dynamic information is not required.
    if next(static_candidates) == nil or next(dynamic_opts) == nil then
        return static_candidates
    end

    -- Filter the remaining candidates after connecting to them.
    connect_to_candidates(static_candidates)
    local dynamic_candidates = {}
    for _, instance_name in pairs(static_candidates) do
        if is_candidate_match_dynamic(instance_name, dynamic_opts) then
            table.insert(dynamic_candidates, instance_name)
        end
    end
    return dynamic_candidates
end

local function get_prefer_local(candidates)
    local preferred_candidates = {}
    local non_preferred_candidates = {}
    for _, candidate in pairs(candidates) do
        if candidate == box.info.name then
            table.insert(preferred_candidates, candidate)
        else
            table.insert(non_preferred_candidates, candidate)
        end
    end
    return preferred_candidates, non_preferred_candidates
end

local function get_prefer_mode(candidates, prefer_mode)
    connect_to_candidates(static_candidates)
    local mode = prefer_mode == 'prefer_ro' and 'ro' or 'rw'
    local preferred_candidates = {}
    local non_preferred_candidates = {}
    for _, candidate in pairs(candidates) do
        local status = statuses[candidate]
        if status ~= nil then
            if status.mode == mode then
                table.insert(preferred_candidates, candidate)
            else
                table.insert(non_preferred_candidates, candidate)
            end
        end
    end
    return preferred_candidates, non_preferred_candidates
end

local function get_connection_to_candidates(candidates)
    while #candidates > 0 do
        local n = math.random(#candidates)
        local instance_name = table.remove(candidates, n)
        local conn = connect(instance_name, {wait_connected = false})
        if conn:wait_connected() then
            return conn
        end
    end
    return nil
end

local function get_connection(opts)
    local mode = nil
    if opts.mode == 'ro' or opts.mode == 'rw' then
        mode = opts.mode
    end
    local candidates_opts = {
        labels = opts.labels,
        roles = opts.roles,
        mode = mode,
    }
    local candidates = filter(candidates_opts)
    if next(candidates) == nil then
        return nil, "no candidates are available with these conditions"
    end

    -- The "prefer_local" option has the highest priority among all the
    -- "prefer_*" options.
    if opts.prefer_local ~= false then
        local preferred_candidates
        preferred_candidates, candidates = get_prefer_local(candidates)
        local conn = get_connection_to_candidates(preferred_candidates)
        if conn ~= nil then
            return conn
        end
    end

    if opts.mode == 'prefer_rw' or opts.mode == 'prefer_ro' then
        local mode = opts.mode
        local preferred_candidates
        preferred_candidates, candidates = get_prefer_mode(candidates, mode)
        local conn = get_connection_to_candidates(preferred_candidates)
        if conn ~= nil then
            return conn
        end
    end

    local conn = get_connection_to_candidates(candidates)
    if conn ~= nil then
        return conn
    end
    return nil, "connection to candidates failed"
end

local function call(func_name, args, opts)
    checks('string', '?table', {
        labels = '?table',
        roles = '?table',
        prefer_local = '?boolean',
        mode = '?string',
        -- The following options passed directly to net.box.call().
        timeout = '?',
        buffer = '?',
        on_push = '?function',
        on_push_ctx = '?',
        is_async = '?boolean',
    })
    opts = opts or {}
    if opts.mode ~= nil and opts.mode ~= 'ro' and opts.mode ~= 'rw' then
        local msg = 'Expected nil, "ro" or "rw", got "%s"'
        error(msg:format(opts.mode), 0)
    end

    local conn_opts = {
        labels = opts.labels,
        roles = opts.roles,
        prefer_local = opts.prefer_local,
        mode = opts.mode,
    }
    local conn, err = get_connection(conn_opts)
    if conn == nil then
        local msg = "Couldn't execute function %s: %s"
        error(msg:format(func_name, err), 0)
    end

    local net_box_call_opts = {
        timeout = opts.timeout,
        buffer = opts.buffer,
        on_push = opts.on_push,
        on_push_ctx = opts.on_push_ctx,
        is_async = opts.is_async,
    }
    return conn:call(func_name, args, net_box_call_opts)
end

return {
    connect = connect,
    filter = filter,
    call = call,
}
