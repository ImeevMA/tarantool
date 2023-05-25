local t = require('luatest')
local justrun = require('test.justrun').tarantool

local g = t.group()

local function imerge(...)
    local result = {}
    for i = 1, select('#', ...) do
        local item = select(i, ...)
        assert(type(item) == 'table')
        for _, v in pairs(item) do
            table.insert(result, v)
        end
    end
    return result
end

local function merge(...)
    local result = {}
    for i = 1, select('#', ...) do
        local item = select(i, ...)
        assert(type(item) == 'table')
        for k, v in pairs(item) do
            result[k] = v
        end
    end
    return result
end

local args = {
    name = {
        cli = {'-n', 'cli-imun'},
        env = {TT_INSTANCE_NAME = 'env-imun'},
    },
    config = {
        cli = {'-c', 'cli.cfg'},
        env = {TT_CONFIG = 'env.cfg'},
    },
    execute = {'-e', [['print("test: Execute Lua")']]},
    interactive = {'-i', '</dev/null'},
}

local results = {
    execute = {
        stdout = {
            output = 'test: Execute Lua',
            pattern = false,
        },
    },
    interactive = {
        stdout = {
            output = 'tarantool>',
            pattern = false,
        },
        stderr = {
            output = "Tarantool [^\n]*\ntype 'help' for interactive help",
            pattern = true,
        },
    },
}

local cases = {
    {
        args = imerge(args.name.cli),
        env = merge(),
    },
    {
        args = imerge(),
        env = merge(args.name.env),
    },
    {
        args = imerge(args.name.cli, args.config.cli),
        env = merge(),
    },
    {
        args = imerge(args.config.cli),
        env = merge(args.name.env),
    },
    {
        args = imerge(args.name.cli),
        env = merge(args.config.env),
    },
    {
        args = imerge(),
        env = merge(args.name.env, args.config.env),
    },
}

g.test_tarantool_run = function()
    for _, case in pairs(cases) do
        local result = justrun('.', case.env, case.args, {nojson = true})
        t.assert_equals(result.exit_code, 0, 'Plain run is successful')
    end
end

g.test_tarantool_run_with_execute = function()
    local expected = results.execute
    for _, case in pairs(cases) do
        local runopts = {nojson = true, stderr = expected.stderr}
        local runargs = imerge(case.args, args.execute)
        local result = justrun('.', case.env, runargs, runopts)
        t.assert_equals(result.exit_code, 0, 'Execute run is successful')
        t.assert_str_contains(result.stdout,
                              expected.stdout.output, expected.stdout.pattern,
                              ('Execute: %s'):format(case.message))
    end
end

g.test_tarantool_run_with_interactive = function()
    local expected = results.interactive
    for _, case in pairs(cases) do
        local runopts = {nojson = true, stderr = expected.stderr}
        local runargs = imerge(case.args, args.interactive)
        local result = justrun('.', case.env, runargs, runopts)
        t.assert_equals(result.exit_code, 0, 'Interactive run is successful')
        t.assert_str_contains(result.stdout,
                              expected.stdout.output, expected.stdout.pattern,
                              ('Interactive: %s (stdout)'):format(case.message))
        t.assert_str_contains(result.stderr,
                              expected.stderr.output, expected.stderr.pattern,
                              ('Interactive: %s (stderr)'):format(case.message))
    end
end

g.test_tarantool_cli_name_arg = function()
    local result = justrun('.', {}, {'-n'}, {nojson = true, stderr = true})
    t.assert_not_equals(result.exit_code, 0, 'Invalid CLI args run is failed')
    t.assert_str_contains(result.stderr, "option requires an argument %-%- 'n'",
                          true, 'Invalid CLI args error')
end

g.test_tarantool_cli_config_arg = function()
    local result = justrun('.', {}, {'-c'}, {nojson = true, stderr = true})
    t.assert_not_equals(result.exit_code, 0, 'Invalid CLI args run is failed')
    t.assert_str_contains(result.stderr, "option requires an argument %-%- 'c'",
                          true, 'Invalid CLI args error')
end
