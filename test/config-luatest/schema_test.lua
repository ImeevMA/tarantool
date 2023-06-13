local t = require('luatest')

local g = t.group()

g.test_1 = function()
    t.assert(require('internal.config.utils.schema'))
end
