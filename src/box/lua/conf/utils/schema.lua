-- Schema manipulations and schema-aware data manipulations.
--
-- The module is fast-written, poorly designed, untested,
-- undocumented. It exists to concentrate schema-aware data
-- manipulations in one place and go forward with other code.
--
-- {{{ Details
--
-- Provides several utility function to costruct a schema
-- definition:
--
-- * schema.new(<record>)
-- * schema.record({
--       foo = <...>,
--   }, {
--       my_annotation = <...>,
--   })
-- * schema.scalar({
--       type = <...>,
--       my_annotation = <...>,
--   })
-- * schema.map({
--       key = schema.scalar(<...>),
--       value = schema.scalar(<...>),
--   })
-- * schema.array({
--       items = schema.scalar(<...>),
--       <..annotations..>
--   })
-- * schema.union_of_records(record_1, record_2, ...)
--
-- There are two auxiliary functions that generate schemas:
--
-- * Create a record, which contains all the fields from two other
--   records.
--   schema.mix(record_1, record_2)
-- * Annotate all (deeply) nested scalars.
--   schema.annotate(<schema object>, {<..annotations..>})
--
-- And the auxiliary function to parse a value declared by a
-- schema node from an environment variable"
--
-- * schema.fromenv(env_var_name, schema_node)
--
-- The schema object provides methods for different purposes.
--
-- * Traversing the schema.
--   <schema object>:pairs() (luafun iterator)
-- * Validate data against the schema.
--   <schema object>:validate(data)
-- * Filter data based on the schema annotations.
--   <schema object>:filter(data, f) (luafun iterator)
-- * Map data based on the schema annotations.
--   <schema object>:map(data, f, f_ctx) -> new_data
-- * Apply default values.
--   <schema object>:apply_default(data) -> new_data
-- * Walk over the data.
--   <schema object>:walkthrough(data, f, f_ctx)
-- * Get/set a nested value.
--   <schema object>:get(data, path)
--   <schema object>:set(data, path, value)
-- * Merge two values.
--   <schema object>:merge(a, b)
--
-- Schema node types (possible `schema.type` values):
--
-- * string
-- * number
-- * integer
-- * union of scalars (say, 'string, number')
-- * record (a dictionary with certain field names and types)
-- * map (arbitrary key names, strict about keys and values types)
-- * array
--
-- }}} Details

-- {{{ Thoughts on future improvements
--
-- Ideas how to improve the module in terms of refactoring of
-- existing functionality.
--
-- * Generalize traversal code and use it for :pairs(), :filter(),
--   :validate().
-- * Some schemas are schema objects (ones produced by
--   schema.new()), while some are not (such as scalars). This is
--   counter-intuitive.
-- * Add schema.enum(def).
--
-- Ideas about new features that would be appreciated.
--
-- * Data shape unaware accessors.
--
--   <schema object>:set(data, {id = <...>}, value)
--   <schema object>:get(data, {id = <...>})
--
--   It allows to write a code that is immutable to data field
--   renames/moves, while IDs are the same.
--
--   Looks as simple to implement, while seems to solve quite
--   big bunch of schema evolution problems.
--
-- * JSON schema generation.
--
--   This is the industry standard and it may be useful for
--   integrations of various kinds.
--
--   It may be also worthful to align schema definition language
--   with the standard to expand reusability of this module.
--
-- Of course, there are a lot of optimization opportunities.
--
-- * Use code generation for at least validation.
-- * Eliminate accumulating of a list to traverse over the schema
--   (:pairs(), :filter()).
-- * Eliminate or replace temporary tables whose only purpose is
--   to pass a couple of values to an external loop (search for
--   `w`).
-- * Prepare data+schema object, which allows to get particular
--   value using <schema object>:get('foo.bar') at one hop.
--
--   local sdata = <schema object>:fill(data)
--   sdata:get('foo.bar') -- one table lookup
--
--   Basically it just flattening of the data like:
--
--   {foo = {bar = 42}} -> {'foo.bar' = 42}
--
--   sdata:get('foo') OTOH involves a table reconstruction. So
--   there are cons and pros.
--
-- }}} Thoughts on future improvements

local fun = require('fun')
local json = require('json')

local schema_mt = {}

local scalars = {}
local methods = {}

-- {{{ Helpers

local function walkthrough_start(self, params)
    local ctx = {path = {}, name = rawget(self, 'name')}
    for k, v in pairs(params or {}) do
        ctx[k] = v
    end
    return ctx
end

local function walkthrough_enter(ctx, name)
    table.insert(ctx.path, name)
end

local function walkthrough_leave(ctx)
    table.remove(ctx.path)
end

local function walkthrough_path(ctx)
    local res = ''
    for _, name in ipairs(ctx.path) do
        if type(name) == 'number' then
            res = res .. ('[%d]'):format(name)
        else
            res = res .. '.' .. name
        end
    end
    return res:sub(2)
end

local function walkthrough_error_prefix(ctx)
    if ctx.path == nil or next(ctx.path) == nil then
        return ('[%s] <no path>'):format(ctx.name)
    end
    return ('[%s] %s'):format(ctx.name, walkthrough_path(ctx))
end

local function walkthrough_error(ctx, message, ...)
    local error_prefix = walkthrough_error_prefix(ctx)
    error(('%s: %s'):format(error_prefix, message:format(...)), 2)
end

-- Verify that data is a table and, if it is not so, produce a
-- nice schema-aware error.
--
-- Applicable for a record, a map, an array.
--
-- Useful as part of validation, but also as a lightweight
-- consistency check.
local function walkthrough_assert_table(ctx, schema, data)
    assert(schema.type == 'record' or schema.type == 'map' or
        schema.type == 'array')

    if type(data) == 'table' then
        return
    end

    local article = schema.type == 'array' and 'an' or 'a'
    walkthrough_error(ctx, 'Unexpected data type for %s %s: %q', article,
        schema.type, type(data))
end

local function is_scalar(schema)
    return scalars[schema.type] ~= nil
end

-- Verify whether given value (data) has expected type and produce
-- a human readable error message otherwise.
local function validate_type_noexc(data, exp_type)
    -- exp_type is like {'string', 'number'}.
    if type(exp_type) == 'table' then
        local found = false
        for _, exp_t in ipairs(exp_type) do
            if type(data) == exp_t then
                found = true
                break
            end
        end
        if not found then
            local exp_type_str = ('"%s"'):format(table.concat(exp_type, '", "'))
            local err = ('Expected one of %s, got %q'):format(exp_type_str,
                type(data))
            return false, err
        end
        return true
    end

    -- exp_type is a Lua type like 'string'.
    assert(type(exp_type) == 'string')
    if type(data) ~= exp_type then
        local err = ('Expected %q, got %q'):format(exp_type, type(data))
        return false, err
    end
    return true
end

-- }}} Helpers

-- {{{ Scalars

-- Scalar types definitions.
--
-- Fields:
--
-- * type (string) -- how the scalar is named
-- * validate_noexc (function) -- check given data against the
--   type constraints
--
--   -> true (means the data is valid)
--   -> false, err (otherwise)
-- * fromenv (function) -- parse data originated from an
--   environment variable

scalars.string = {
    type = 'string',
    validate_noexc = function(data)
        return validate_type_noexc(data, 'string')
    end,
    fromenv = function(_env_var_name, raw_value)
        return raw_value
    end,
}

scalars.number = {
    type = 'number',
    validate_noexc = function(data)
        -- TODO: Should we accept cdata<int64_t> and
        -- cdata<uint64_t> here?
        return validate_type_noexc(data, 'number')
    end,
    fromenv = function(env_var_name, raw_value)
        -- TODO: Accept large integers and return cdata<int64_t>
        -- or cdata<uint64_t>?
        local res = tonumber(raw_value)
        if res == nil then
            error(('Unable to decode a number value from environment ' ..
                'variable %q, got %q'):format(env_var_name, raw_value))
        end
        return res
    end,
}

scalars.integer = {
    type = 'integer',
    validate_noexc = function(data)
        -- TODO: Accept cdata<int64_t> and cdata<uint64_t>.
        local ok, err = validate_type_noexc(data, 'number')
        if not ok then
            return false, err
        end
        if data - math.floor(data) ~= 0 then
            local err = ('Expected number without a fractional part, ' ..
                'got %d'):format(data)
            return false, err
        end
        return true
    end,
    fromenv = function(env_var_name, raw_value)
        local res = tonumber64(raw_value)
        if res == nil then
            error(('Unable to decode an integer value from environment ' ..
                'variable %q, got %q'):format(env_var_name, raw_value))
        end
        return res
    end,
}

-- TODO: This hack is needed until a union of scalars will be
-- implemented.
scalars['string, number'] = {
    type = 'string, number',
    validate_noexc = function(data)
        return validate_type_noexc(data, {'string', 'number'})
    end,
    fromenv = function(_env_var_name, raw_value)
        return tonumber(raw_value) or raw_value
    end,
}
scalars['number, string'] = {
    type = 'number, string',
    validate_noexc = function(data)
        return validate_type_noexc(data, {'string', 'number'})
    end,
    fromenv = function(_env_var_name, raw_value)
        return tonumber(raw_value) or raw_value
    end,
}

scalars.boolean = {
    type = 'boolean',
    validate_noexc = function(data)
        return validate_type_noexc(data, 'boolean')
    end,
    fromenv = function(env_var_name, raw_value)
        -- Accept false/true case insensitively.
        --
        -- Accept 0/1 as boolean values.
        if raw_value:lower() == 'false' or raw_value == '0' then
            return false
        end
        if raw_value:lower() == 'true' or raw_value == '1' then
            return true
        end

        error(('Unable to decode a boolean value from environment ' ..
            'variable %q, got %q'):format(env_var_name, raw_value))
    end,
}

-- }}} Scalars

-- {{{ Instance methods

local schema_pairs_impl
schema_pairs_impl = function(schema, ctx)
    if is_scalar(schema) then
        local w = {
            path = table.copy(ctx.path),
            schema = schema,
        }
        table.insert(ctx.acc, w)
    elseif schema.type == 'record' then
        for k, v in pairs(schema.fields) do
            walkthrough_enter(ctx, k)
            schema_pairs_impl(v, ctx)
            walkthrough_leave(ctx)
        end
    elseif schema.type == 'map' then
        assert(schema.key ~= nil)
        assert(schema.value ~= nil)
        local w = {
            path = table.copy(ctx.path),
            schema = schema,
        }
        table.insert(ctx.acc, w)
    elseif schema.type == 'array' then
        assert(schema.items ~= nil)
        local w = {
            path = table.copy(ctx.path),
            schema = schema,
        }
        table.insert(ctx.acc, w)
    else
        assert(false)
    end
end

-- Walk over the schema and return scalars, arrays and maps.
--
--  | for _, node in schema:pairs() do
--  |     local path = node.path
--  |     local type = node.type
--  |     <...>
--  | end
--
-- TODO: Rewrite it without collecting a list beforehand.
function methods.pairs(self)
    local ctx = walkthrough_start(self, {acc = {}})
    schema_pairs_impl(rawget(self, 'schema'), ctx)
    return fun.iter(ctx.acc)
end

-- The path can be passed as a string in dot notation or as a
-- table representing an array of components. This function
-- converts the path into the array if necessary.
local function normalize_path(path, error_f)
    if type(path) ~= 'string' and type(path) ~= 'table' then
        return error_f()
    end

    -- Dot notation/JSON path alike.
    --
    -- TODO: Support numeric indexing: [1], [2] and so on.
    if type(path) == 'string' then
        if path == '' then
            -- XXX: Is it right way to handle the empty string?
            path = {}
        else
            path = path:split('.')
        end
    end

    return path
end

-- local data = {}
-- schema:set(data, 'foo.bar', 42)
-- print(data.foo.bar) -- 42
function methods.set(_self, data, path, value)
    local function usage()
        error('Usage: schema:set(data: table, path: string/table, value: any)')
    end

    if type(data) ~= 'table' then
        return usage()
    end

    path = normalize_path(path, usage)

    if path == nil or next(path) == nil then
        error('schema:set: empty path')
    end

    -- TODO: Forbid paths that are not in the schema.
    local cur = data
    for i, component in ipairs(path) do
        if i < #path then
            if cur[component] == nil then
                cur[component] = {}
            end
        else
            cur[component] = value
        end
        cur = cur[component]
    end
end

local get_impl
get_impl = function(schema, data, ctx)
    -- The journey is finished. Return what is under the feet.
    if #ctx.journey == 0 then
        return data
    end

    -- There are more steps in the journey (at least one).
    -- Let's dive deeper and process it per schema node type.

    local requested_field = ctx.journey[1]
    assert(requested_field ~= nil)

    if is_scalar(schema) then
        walkthrough_error(ctx, 'Attempt to index scalar by %q',
            requested_field)
    elseif schema.type == 'record' then
        walkthrough_enter(ctx, requested_field)
        local field_def = schema.fields[requested_field]
        if field_def == nil then
            walkthrough_error(ctx, 'No such field in the schema')
        end

        -- Even if there is no such field in the data, continue
        -- the descending to validate the path against the schema.
        local field_value
        if data ~= nil then
            field_value = data[requested_field]
        end

        table.remove(ctx.journey, 1)
        return get_impl(field_def, field_value, ctx)
    elseif schema.type == 'map' then
        walkthrough_enter(ctx, requested_field)
        local field_def = schema.value

        -- Even if there is no such field in the data, continue
        -- the descending to validate the path against the schema.
        local field_value
        if data ~= nil then
            field_value = data[requested_field]
        end

        table.remove(ctx.journey, 1)
        return get_impl(field_def, field_value, ctx)
    elseif schema.type == 'array' then
        -- TODO: Support 'foo[1]' and `{'foo', 1}` paths. See the
        -- normalize_path() function.
        walkthrough_error(ctx, 'Indexing an array is not supported yet')
    else
        assert(false)
    end
end

-- local data = {foo = {bar = 'x'}}
-- schema:get(data, 'foo.bar') -> 'x'
-- schema:get(data, {'foo', 'bar'}) -> 'x'
function methods.get(self, data, path)
    local function usage()
        error('Usage: schema:get(data: table, path: nil/string/table)')
    end

    if type(data) ~= 'table' then
        return usage()
    end

    if path ~= nil then
        path = normalize_path(path, usage)
    end

    if path == nil or next(path) == nil then
        return data
    end

    local ctx = walkthrough_start(self, {
        -- The `path` field is already in the context and it means
        -- passed path. Let's name the remaining path as
        -- `journey`.
        journey = path,
    })
    return get_impl(rawget(self, 'schema'), data, ctx)
end

local walkthrough_impl
walkthrough_impl = function(schema, data, f, ctx)
    local w = {
        path = ctx.path,
        schema = schema,
        data = data,
    }
    f(w, ctx.f_ctx)

    -- luacheck: ignore 542 empty if branch
    if is_scalar(schema) then
        -- Nothing to do.
    elseif schema.type == 'record' then
        walkthrough_assert_table(ctx, schema, data)

        for field_name, field_def in pairs(schema.fields) do
            walkthrough_enter(ctx, field_name)

            if type(field_def) ~= 'table' then
                walkthrough_error(ctx, 'Unexpected schema node type %q',
                    type(field_def))
            end

            local field = data[field_name]
            -- Assume fields as non-required.
            if field ~= nil then
                walkthrough_impl(field_def, field, f, ctx)
            end

            walkthrough_leave(ctx)
        end
    elseif schema.type == 'map' then
        walkthrough_assert_table(ctx, schema, data)

        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            -- XXX: Ignore keys? Or add them to the `w`
            -- (walkthrough node) generated in the inner call
            -- using the context?
            walkthrough_impl(schema.value, field_value, f, ctx)
            walkthrough_leave(ctx)
        end
    elseif schema.type == 'array' then
        walkthrough_assert_table(ctx, schema, data)

        for i, v in ipairs(data) do
            walkthrough_enter(ctx, i)
            walkthrough_impl(schema.items, v, f, ctx)
            walkthrough_leave(ctx)
        end
    else
        assert(false)
    end
end

function methods.walkthrough(self, data, f, f_ctx)
    local ctx = walkthrough_start(self, {f_ctx = f_ctx})
    walkthrough_impl(rawget(self, 'schema'), data, f, ctx)
end

function methods.filter(self, data, filter_f)
    local acc = {}
    self:walkthrough(data, function(w)
        if filter_f(w) then
            table.insert(acc, w)
        end
    end)
    return fun.iter(acc)
end

local function validate_impl(schema, data, ctx)
    if is_scalar(schema) then
        local scalar_def = scalars[schema.type]
        assert(scalar_def ~= nil)
        local ok, err = scalar_def.validate_noexc(data)
        if not ok then
            -- TODO: We'll likely allow schema.type to be a table
            -- later. At this point the generation of the error
            -- message should be adjusted.
            assert(type(schema.type) == 'string')
            walkthrough_error(ctx, "Unexpected data for scalar %q: %s",
                schema.type, err)
        end
    elseif schema.type == 'record' then
        walkthrough_assert_table(ctx, schema, data)

        for field_name, field_def in pairs(schema.fields) do
            walkthrough_enter(ctx, field_name)

            local field = data[field_name]
            -- Assume fields as non-required.
            if field ~= nil then
                validate_impl(field_def, field, ctx)
            end

            walkthrough_leave(ctx)
        end

        -- Walk over the data to catch unknown fields.
        for field_name, _ in pairs(data) do
            local field_def = schema.fields[field_name]
            if field_def == nil then
                walkthrough_error(ctx, 'Unexpected field "%s"', field_name)
            end
        end
    elseif schema.type == 'map' then
        walkthrough_assert_table(ctx, schema, data)

        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            validate_impl(schema.key, field_name, ctx)
            validate_impl(schema.value, field_value, ctx)
            walkthrough_leave(ctx)
        end
    elseif schema.type == 'array' then
        walkthrough_assert_table(ctx, schema, data)

        -- Check that all the keys are numeric.
        local key_count = 0
        local min_key = 1/0  -- +inf
        local max_key = -1/0 -- -inf
        for k, _ in pairs(data) do
            if type(k) ~= 'number' then
                walkthrough_error(ctx, 'An array contains a non-numeric ' ..
                    'key: %q', k)
            end
            key_count = key_count + 1
            min_key = math.min(min_key, k)
            max_key = math.max(max_key, k)
        end

        -- NB: An empty array is a valid array, so it is excluded
        -- from the checks below.

        -- Check that the array starts from 1 and has no holes.
        if key_count ~= 0 and min_key ~= 1 then
            walkthrough_error(ctx, 'An array must start from index 1, ' ..
                'got min index %d', min_key)
        end

        if key_count ~= 0 and max_key ~= key_count then
            walkthrough_error(ctx, 'An array must not have holes, got ' ..
                'a table with %d numeric fields with max index %d', key_count,
                max_key)
        end

        for i, v in ipairs(data) do
            walkthrough_enter(ctx, i)
            validate_impl(schema.items, v, ctx)
            walkthrough_leave(ctx)
        end
    else
        assert(false)
    end

    -- Call user provided validation function.
    --
    -- Important: it is called when all the type validation is
    -- already done, including nested nodes.
    if schema.validate ~= nil then
        assert(type(schema.validate) == 'function')
        local w = {
            path = ctx.path,
            error = function(message, ...)
                walkthrough_error(ctx, message, ...)
            end,
        }
        schema.validate(schema, data, w)
    end
end

function methods.validate(self, data)
    local ctx = walkthrough_start(self)
    validate_impl(rawget(self, 'schema'), data, ctx)
end

local merge_impl
merge_impl = function(schema, a, b, ctx)
    -- There is no value at one of the sides -- pick up another
    -- one.
    if a == nil then
        return b
    end
    if b == nil then
        return a
    end

    -- Scalars and arrays are not to be merged.
    --
    -- At this point neither `a`, nor `b` is `nil`, so
    -- return the preferred value, `b`.
    if is_scalar(schema) then
        return b
    elseif schema.type == 'array' then
        walkthrough_assert_table(ctx, schema, a)
        walkthrough_assert_table(ctx, schema, b)

        return b
    end

    -- `a` and `b` are both non-nil records or maps. Perform the
    -- deep merge.
    if schema.type == 'record' then
        walkthrough_assert_table(ctx, schema, a)
        walkthrough_assert_table(ctx, schema, b)

        local res = {}
        for field_name, field_def in pairs(schema.fields) do
            walkthrough_enter(ctx, field_name)
            local a_field = a[field_name]
            local b_field = b[field_name]
            res[field_name] = merge_impl(field_def, a_field, b_field, ctx)
            walkthrough_leave(ctx)
        end
        return res
    elseif schema.type == 'map' then
        walkthrough_assert_table(ctx, schema, a)
        walkthrough_assert_table(ctx, schema, b)

        local res = {}
        for field_name, a_field in pairs(a) do
            walkthrough_enter(ctx, field_name)
            local b_field = b[field_name]
            res[field_name] = merge_impl(schema.value, a_field, b_field, ctx)
            walkthrough_leave(ctx)
        end
        -- NB: No error is possible, so let's skip
        -- walkthrough_enter()/walkthrough_leave().
        for field_name, b_field in pairs(b) do
            if a[field_name] == nil then
                res[field_name] = b_field
            end
        end
        return res
    else
        assert(false)
    end
end

-- Merge two hierarical values.
--
-- Prefer the latter.
--
-- Records and maps are deeply merged. Scalars and arrays are
-- all-or-nothing: either one is choosen or another.
function methods.merge(self, a, b)
    local ctx = walkthrough_start(self)
    return merge_impl(rawget(self, 'schema'), a, b, ctx)
end

local function map_impl(schema, data, f, ctx)
    if is_scalar(schema) then
        local w = {
            path = ctx.path,
            error = function(message, ...)
                walkthrough_error(ctx, message, ...)
            end,
        }
        return f(schema, data, w, ctx.f_ctx)
    elseif schema.type == 'record' then
        if data ~= nil then
            walkthrough_assert_table(ctx, schema, data)
        end

        local res = {}
        for field_name, field_def in pairs(schema.fields) do
            walkthrough_enter(ctx, field_name)

            local field
            if data ~= nil then
                field = data[field_name]
            end
            res[field_name] = map_impl(field_def, field, f, ctx)

            walkthrough_leave(ctx)
        end
        if next(res) == nil and data == nil then
            return nil
        end
        return res
    elseif schema.type == 'map' then
        if data == nil then
            return nil
        end

        walkthrough_assert_table(ctx, schema, data)

        local res = {}
        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            local new_field_name = map_impl(schema.key, field_name, f, ctx)
            local new_field_value = map_impl(schema.value, field_value, f, ctx)
            res[new_field_name] = new_field_value
            walkthrough_leave(ctx)
        end
        return res
    elseif schema.type == 'array' then
        if data == nil then
            return nil
        end

        walkthrough_assert_table(ctx, schema, data)

        local res = {}
        for i, v in ipairs(data) do
            walkthrough_enter(ctx, i)
            local new_item_value = map_impl(schema.items, v, f, ctx)
            res[i] = new_item_value
            walkthrough_leave(ctx)
        end
        return res
    else
        assert(false)
    end
end

-- Transform data by the given function.
--
-- Leave the shape of the data unchanged.
--
-- An example of a mapping function:
--
--  | local function m(schema, data, _w)
--  |     if schema.type == 'string' and data ~= nil then
--  |         return data:gsub('X', 'Y')
--  |     end
--  |     return data
--  | end
--
-- Nuances:
--
-- * The function is called only for scalars.
function methods.map(self, data, f, f_ctx)
    local ctx = walkthrough_start(self, {f_ctx = f_ctx})
    return map_impl(rawget(self, 'schema'), data, f, ctx)
end

local function apply_default_f(schema, data, _w)
    if data == nil then
        return schema.default
    end
    return data
end

function methods.apply_default(self, data)
    return self:map(data, apply_default_f)
end

function schema_mt.__index(self, key)
    local instance_methods = rawget(self, 'methods')
    if instance_methods[key] ~= nil then
        return instance_methods[key]
    end
    if methods[key] ~= nil then
        return methods[key]
    end
    return rawget(self, key)
end

-- }}} Instance methods

-- {{{ Module functions

-- Forward declarations.
local mix

-- schema.scalar({
--     type = 'string',
--     my_annotation = <...>,
-- })
local function scalar(scalar_def)
    assert(scalar_def.type ~= nil)
    return scalar_def
end

-- schema.record({
--     foo = schema.scalar(<...>),
-- }, {
--     <..annotations..>
-- })
local function record(fields, annotations)
    local res = {
        type = 'record',
        fields = fields or {},
    }
    for k, v in pairs(annotations or {}) do
        res[k] = v
    end
    return res
end

local function map(map_def)
    assert(map_def.key ~= nil)
    assert(map_def.value ~= nil)
    local res = table.copy(map_def)
    res.type = 'map'
    return res
end

local function array(array_def)
    assert(array_def.items ~= nil)
    local res = table.copy(array_def)
    res.type = 'array'
    return res
end

local function new(name, schema, opts)
    opts = opts or {}
    local instance_methods = opts.methods or {}

    return setmetatable({
        name = name,
        schema = schema,
        methods = instance_methods,
    }, schema_mt)
end

-- Union of several records.
--
-- The data can contain either one record or another.
--
-- TODO: This is a stub function: it lacks validation that fields
-- from different records are not present simultaneously. This can
-- be implemented, but there are prerequisites:
--
-- 1. Modify record's representation from just...
--
--    {field_1 = <schema>, field_2 = <schema>}
--
--    ...to...
--
--    {
--        fields = {field_1 = <schema>, field_2 = <schema>},
--        constrainsts = <...>,
--    }
-- 2. [optional, but desirable] Add schema.record() constructor.
-- 3. Fill the `constraints` field (or whatever it'll be named)
--    with constraints about allowed keys in the
--    union_of_records() function.
-- 4. Support the `contraints` field in
--    <schema object>:validate().
local function union_of_records(...)
    local res = record({})
    for i = 1, select('#', ...) do
        res = mix(res, (select(i, ...)))
    end
    return res
end

-- Create a record, which is composition of fields from two records.
mix = function(a, b)
    assert(type(a) == 'table')
    assert(type(b) == 'table')

    -- Accept a schema object as a record.
    if getmetatable(a) == schema_mt then
        a = rawget(a, 'schema')
    end
    if getmetatable(b) == schema_mt then
        b = rawget(b, 'schema')
    end

    assert(type(a) == 'table')
    assert(type(b) == 'table')
    assert(a.type == 'record')
    assert(b.type == 'record')

    local fields = {}
    local annotations = {}

    for field_name, field_def in pairs(a.fields) do
        fields[field_name] = field_def
    end
    for k, v in pairs(a) do
        if k ~= 'fields' and k ~= 'type' then
            annotations[k] = v
        end
    end

    for field_name, field_def in pairs(b.fields) do
        if fields[field_name] ~= nil then
            -- XXX: Proper error reporting.
            error('XXX')
        end
        fields[field_name] = field_def
    end
    for k, v in pairs(b) do
        if k ~= 'fields' and k ~= 'type' then
            if annotations[k] ~= nil then
                -- XXX: Proper error reporting.
                error('XXX')
            end
            annotations[k] = v
        end
    end

    return record(fields, annotations)
end

local annotate_impl
annotate_impl = function(schema, ctx)
    local res = table.copy(schema)

    if is_scalar(schema) then
        for k, v in pairs(ctx.annotations) do
            res[k] = v
        end
    elseif schema.type == 'record' then
        for field_name, field_def in pairs(schema.fields) do
            walkthrough_enter(ctx, field_name)
            res[field_name] = annotate_impl(field_def, ctx)
            walkthrough_leave(ctx)
        end
    elseif schema.type == 'map' then
        res.key = annotate_impl(schema.key, ctx)
        res.value = annotate_impl(schema.value, ctx)
    elseif schema.type == 'array' then
        res.items = annotate_impl(schema.items, ctx)
    else
        assert(false)
    end

    return res
end

-- Add given annotations to each scalar in the given schema.
local function annotate(schema, annotations)
    assert(type(schema) == 'table')
    assert(type(annotations) == 'table')

    -- Accept a schema object as a record/scalar.
    if getmetatable(schema) == schema_mt then
        schema = rawget(schema, 'schema')
    end

    -- Fast schema check.
    assert(type(schema) == 'table')
    assert(schema.type == 'record' or schema.type == 'map' or is_scalar(schema))

    -- Traverse over the schema and add the annotations to each
    -- scalar.
    local ctx = {path = {}, name = '<unknown>', annotations = annotations}
    return annotate_impl(schema, ctx)
end

local fromenv
fromenv = function(env_var_name, raw_value, schema)
    if raw_value == nil or raw_value == '' then
        return nil
    end

    if is_scalar(schema) then
        local scalar_def = scalars[schema.type]
        assert(scalar_def ~= nil)
        return scalar_def.fromenv(env_var_name, raw_value)
    elseif schema.type == 'record' then
        error(('Unable to get a record value from environment variable %q: ' ..
            'records are not implemented'):format(env_var_name))
    elseif schema.type == 'map' then
        if schema.key.type ~= 'string' then
            error(('Unable to get a map value from environment variable %q: ' ..
                'non-string key types are not implemented'):format(
                env_var_name))
        end

        -- JSON object.
        if raw_value:startswith('{') then
            local ok, res = pcall(json.decode, raw_value)
            if not ok then
                error(('Unable to decode JSON data in environment ' ..
                    'variable %q: %s'):format(env_var_name, res))
            end
            return res
        end

        -- JSON array.
        if raw_value:startswith('[') then
            error(('JSON array is provided for environment variable %q of ' ..
                'type map'):format(env_var_name))
        end

        -- foo=bar,baz=fiz
        local res = {}
        for _, v in ipairs(raw_value:split(',')) do
            local eq = v:find('=')
            if eq == nil then
                error(('Expected JSON or foo=bar,fiz=baz format for ' ..
                    'environment variable %q'):format(env_var_name))
            end
            local lhs = string.sub(v, 1, eq - 1)
            local rhs = string.sub(v, eq + 1)

            if lhs == '' then
                error(('Expected JSON or foo=bar,fiz=baz format for ' ..
                    'environment variable %q'):format(env_var_name))
            end
            local subname = ('%s.%s'):format(env_var_name, lhs)
            res[lhs] = fromenv(subname, rhs, schema.value)
        end
        return res
    elseif schema.type == 'array' then
        -- JSON array.
        if raw_value:startswith('[') then
            local ok, res = pcall(json.decode, raw_value)
            if not ok then
                error(('Unable to decode JSON data in environment ' ..
                    'variable %q: %s'):format(env_var_name, res))
            end
            return res
        end

        -- JSON object.
        if raw_value:startswith('{') then
            error(('JSON object is provided for environment variable %q of ' ..
                'type array'):format(env_var_name))
        end

        local res = {}
        for i, v in ipairs(raw_value:split(',')) do
            local subname = ('%s[%d]'):format(env_var_name, i)
            res[i] = fromenv(subname, v, schema.items)
        end
        return res
    else
        assert(false)
    end
end

-- }}} Module functions

return {
    -- Schema node constructors.
    scalar = scalar,
    record = record,
    map = map,
    array = array,

    -- Schema object constructor.
    --
    -- It creates an object with methods from a schema node.
    new = new,

    -- Constructors for 'derived types'.
    --
    -- It produces a scalar, record, map or array, but annotates
    -- it in some specific way to, say, impose extra constraint
    -- rules at validation.
    --
    -- TODO: add .enum()
    union_of_records = union_of_records,

    -- Schema/schema node modification/tranformation functions.
    mix = mix,
    annotate = annotate,

    -- Schema aware data parsers.
    fromenv = fromenv,
}
