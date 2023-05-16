local log = require('conf.utils.log')

local function apply(configdata)
    configdata:filter(function(w)
        return w.schema.mkdir ~= nil
    end):map(function(w)
        return w.path, w.data
    end):each(function(path, dir)
        log.debug('mkdir.apply: %s %s', path, dir)
        -- XXX: Actually create the directory (if needed).
    end)
end

return {
    name = 'mkdir',
    apply = apply,
}
