local privileges = require('internal.config.privileges')

local function apply(config_module)
    privileges.set_aboard(config_module._aboard)
    privileges.set('config', config_module._configdata:get('credentials'))
end

return {
    name = 'credentials',
    apply = apply,
}
