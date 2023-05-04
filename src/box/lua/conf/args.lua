local fun = require('fun')
local argparse = require('internal.argparse')

local args_def = {
    {
        env = 'TT_INSTANCE_NAME',
        dest = 'instance_name',
    },
    {
        env = 'TT_CONFIG',
        dest = 'config_file',
    },
}

local function parse(instance_name, config_file)
    local ht = {
        instance_name = instance_name,
        config_file = config_file,
    }
    return fun.iter(args_def):map(function(def)
        return def.dest, os.getenv(def.env) or ht[def.dest]
    end):tomap()
end

return {
    parse = parse,
}
