local console = require('console')
local log = require('internal.conf.utils.log')

local function socket_file_to_listen_uri(file)
    if file:startswith('/') or file:startswith('./') then
        return ('unix/:%s'):format(file)
    end
    return ('unix/:./%s'):format(file)
end

local function apply(configdata)
    local enabled = configdata:get('console.enabled', {use_default = true})
    if not enabled then
        log.debug('console.apply: console is disabled by the ' ..
            'console.enabled option; skipping...')
        return
    end

    local socket_file = configdata:get('console.socket', {use_default = true})
    assert(socket_file ~= nil)

    local listen_uri = socket_file_to_listen_uri(socket_file)
    log.debug('console.apply: %s', listen_uri)

    -- The default value points to a system directory and it is
    -- fine if we got a permission denied error at attempt to
    -- bind to a Unix domain socket in the system directory.
    --
    -- There is no good way to differentiate errors (except
    -- matching strerror() strings, which may vary between
    -- libc implementations). Let's assume any error as
    -- non-fatal.
    --
    -- TODO: Ignore EACCES, report other errors.
    local ok, res = pcall(console.listen, listen_uri)
    if not ok then
        -- TODO: Add a warning into conf.issues(), when it will
        -- be implemented.
        local msg = 'unable to bind console socket %q: %s'
        log.warn('console.apply: ' .. msg, listen_uri, res)
    end
end

return {
    name = 'console',
    apply = apply,
}
