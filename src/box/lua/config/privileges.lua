local credentials = require('internal.config.applier.credentials')

local methods = {}
local mt = {
    __index = methods,
}

function methods.set(self, name, creds)
    credentials._set(name, creds)
end

return setmetatable({}, mt)
