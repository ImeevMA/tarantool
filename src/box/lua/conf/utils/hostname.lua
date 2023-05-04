local ffi = require('ffi')
local errno = require('errno')

-- Ignore errors to make the code reloadable.
pcall(ffi.cdef, 'int gethostname(char *name, size_t len);')

local function get()
    local buf_size = 256
    local buf = ffi.new('char[?]', buf_size)
    local rc = ffi.C.gethostname(buf, buf_size)
    if rc ~= 0 then
        error(('gethostname: %s'):format(errno.strerror()), 0)
    end
    return ffi.string(buf)
end

return {
    get = get,
}
