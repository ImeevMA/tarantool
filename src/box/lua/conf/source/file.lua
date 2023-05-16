local fio = require('fio')
local yaml = require('yaml')

local values = {}

local function sync(ctx, _iconfig)
    assert(ctx.config_file ~= nil)

    local fh, err = fio.open(ctx.config_file)
    if fh == nil then
        error(('Unable to open a config file %q: %s'):format(
            ctx.config_file, err))
    end
    local data = fh:read()
    fh:close()

    local ok, res = pcall(yaml.decode, data)
    if not ok then
        error(('Unable to parse a config file %q as YAML: %s'):format(
            ctx.config_file, res))
    end

    values = res
end

local function get()
    return values
end

return {
    name = 'file',
    -- The type is either 'instance' or 'cluster'.
    type = 'cluster',
    -- Gather most actual config values.
    sync = sync,
    -- Access the configuration after source.sync().
    --
    -- source.get()
    get = get,
}
