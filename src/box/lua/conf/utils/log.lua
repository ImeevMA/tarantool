-- Logger wrapper with a few enhancements.
--
-- 1. Encode tables into JSON.
-- 2. Enable all the messages on TT_CONF_DEBUG=1.

local json_noexc = require('json').new()
json_noexc.cfg({encode_use_tostring = true})

local logger_name = 'tarantool.conf'
local log = require('log').new(logger_name)

local func2level = {
    [log.error] = 2,
    [log.warn] = 3,
    [log.info] = 5,
    [log.verbose] = 6,
    [log.debug] = 7,
}

local func2prefix = {
    [log.error] = 'E> ',
    [log.warn] = 'W> ',
    [log.info] = 'I> ',
    [log.verbose] = 'V> ',
    [log.debug] = 'D> ',
}

local str2level = {
    ['error'] = 2,
    ['warn'] = 3,
    ['info'] = 5,
    ['verbose'] = 6,
    ['debug'] = 7,
}

local function say_closure(log_f)
    local prefix = ''

    -- Enable logging of everything if an environment variable is
    -- set.
    --
    -- Useful for debugging.
    --
    -- Just setting of...
    --
    -- ```
    -- TT_LOG_MODULES='{"tarantool.conf": "debug"}'
    -- ```
    --
    -- ...is not suitable due to gh-8092: messages before first
    -- box.cfg() are not shown.
    --
    -- Explicit calling of...
    --
    -- ```
    -- log.cfg({modules = {['tarantool.conf'] = 'debug'}})
    -- ```
    --
    -- is not suitable as well, because it makes the logger
    -- already configured and non-dynamic options like
    -- `box_cfg.log_nonblock` can't be applied at first box.cfg()
    -- invocation.
    --
    -- So just prefix our log messages and use log.info().
    local envvar = os.getenv('TT_CONF_DEBUG')
    if envvar ~= nil and envvar ~= '0' and envvar ~= 'false' then
        prefix = func2prefix[log_f]
        log_f = log.info
    end

    return function(fmt, ...)
        -- Skip logging based on the log level.
        --
        -- log.cfg is the main logger configuration. See gh-8610.
        local level = log.cfg.modules and log.cfg.modules[logger_name] and
            log.cfg.modules[logger_name] or log.cfg.level
        level = str2level[level] or level
        assert(type(level) == 'number')
        if func2level[log_f] > level then
            return
        end

        -- Micro-optimization: don't create a temporary table if
        -- it is not needed.
        local argc = select('#', ...)
        if argc == 0 then
            log_f(fmt)
            return
        end

        -- Encode tables into JSON.
        --
        -- Ignores presence of __serialize and __tostring in the
        -- metatatable. It is suitable for the conf module needs.
        local args = {...}
        for i = 1, argc do
            if type(args[i]) == 'table' then
                args[i] = json_noexc.encode(args[i])
            end
        end

        -- Pass the result to the logger function.
        log_f(prefix .. fmt, unpack(args, 1, argc))
    end
end

return {
    error = say_closure(log.error),
    warn = say_closure(log.warn),
    info = say_closure(log.info),
    verbose = say_closure(log.verbose),
    debug = say_closure(log.debug),
}
