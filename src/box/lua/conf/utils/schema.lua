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
--   <schema object>:get(data, path, opts)
--   <schema object>:set(data, path, value)
-- * Compare values.
--   <schema object>:equals(a, b)
-- * Merge two values.
--   <schema object>:merge(a, b)
--
-- Schema node types (possible `schema.type` values):
--
-- * string
-- * number
-- * integer
-- * [string] (yeah, that's the temporary hack)
-- * union of scalars (say, 'string, number')
-- * record (a dictionary with certain field names and types)
-- * map (arbitrary key names, strict about keys and values types)
--
-- }}} Details

-- {{{ Thoughts on future improvements
--
-- Ideas how to improve the module in terms of refactoring of
-- existing functionality.
--
-- * Generalize traversal code and use it for :pairs(), :filter(),
--   :validate().
-- * Define scalar types explicitly and write validation functions
--   for the types inside the scaral definition object.
-- * Some schemas are schema objects (ones produced by
--   schema.new()), while some are not (such as scalars). This is
--   counter-intuitive.
-- * A record definition can't have a metatable, even one produced
--   by default by {json,yaml,msgpack}.decode(). Consider another
--   way to mark node types. For example,
--
--   schema.record(def) -> {type = 'record', fields = def}
--   schema.scalar(def) -> {type = 'scalar', scalar = def}
-- * Add schema.array(def).
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

local function is_scalar(schema)
    return scalars[schema.type] ~= nil
end

local function x_or_default(x, default, opts)
    if x ~= nil then
        return x
    end
    if opts ~= nil and opts.use_default then
        return default
    end
    return nil
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

scalars.string = {
    type = 'string',
    validate_noexc = function(data)
        return validate_type_noexc(data, 'string')
    end,
}

scalars.number = {
    type = 'number',
    validate_noexc = function(data)
        -- TODO: Should we accept cdata<int64_t> and
        -- cdata<uint64_t> here?
        return validate_type_noexc(data, 'number')
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
}

-- TODO: This hack is needed until schema.array() will be
-- implemented.
scalars['[string]'] = {
    type = '[string]',
    validate_noexc = function(data)
        -- XXX: Check whether it is an array (all keys are numeric, starts
        -- from 1, no holes)
        -- XXX: Check that all the array items are strings.
        return validate_type_noexc(data, 'table')
    end,
}

-- TODO: This hack is needed until a union of scalars will be
-- implemented.
scalars['string, number'] = {
    type = 'string, number',
    validate_noexc = function(data)
        return validate_type_noexc(data, {'string', 'number'})
    end,
}
scalars['number, string'] = {
    type = 'number, string',
    validate_noexc = function(data)
        return validate_type_noexc(data, {'string', 'number'})
    end,
}

scalars.boolean = {
    type = 'boolean',
    validate_noexc = function(data)
        return validate_type_noexc(data, 'boolean')
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
    else
        assert(false)
    end
end

-- Walk over the schema and return scalars and maps.
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
    --
    -- Note: only scalars can have a default for now. At least I
    -- don't know for sure how to better define behavior for
    -- defaults in composite types, which may have descendants with
    -- its own defaults.
    --
    -- Anyway, we access the `schema.default` field unconditionally
    -- with assumption that it is present only in scalars.
    if #ctx.journey == 0 then
        return x_or_default(data, schema.default, ctx)
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
        -- the descending to reach the default (if any).
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
        -- the descending to reach the default (if any).
        local field_value
        if data ~= nil then
            field_value = data[requested_field]
        end

        table.remove(ctx.journey, 1)
        return get_impl(field_def, field_value, ctx)
    else
        assert(false)
    end
end

-- local data = {foo = {bar = 'x'}}
-- schema:get(data, 'foo.bar') -> 'x'
-- schema:get(data, {'foo', 'bar'}) -> 'x'
function methods.get(self, data, path, opts)
    opts = opts or {}

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
        use_default = opts.use_default,
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
        if type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a record: %q',
                type(data))
        end

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
        if type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a map: %q',
                type(data))
        end

        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            -- XXX: Ignore keys? Or add them to the `w`
            -- (walkthrough node) generated in the inner call
            -- using the context?
            walkthrough_impl(schema.value, field_value, f, ctx)
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
        if w.schema.type == 'record' then
            return
        end
        if w.schema.type == 'map' then
            return
        end
        assert(is_scalar(w.schema))
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
        if type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a record: %q',
                type(data))
        end

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
        if type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a map: %q',
                type(data))
        end

        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            validate_impl(schema.key, field_name, ctx)
            validate_impl(schema.value, field_value, ctx)
            walkthrough_leave(ctx)
        end
    else
        assert(false)
    end
end

function methods.validate(self, data)
    local ctx = walkthrough_start(self)
    validate_impl(rawget(self, 'schema'), data, ctx)
end

-- Whether two values are equal.
--
-- It is equivalent to a simple deep compare, but placed to the
-- schema object for convenience.
--
-- May assume that both values are conform to the schema.
--
-- May use the schema knowledge for optimizations using, say, code
-- generation.
function methods.equals(_self, _a, _b)
    -- TODO: Implement.
    return false
end

local merge_impl
merge_impl = function(a, b)
    if type(a) ~= 'table' then
        return b
    end
    if b == nil then
        return a
    end
    assert(type(b) == 'table')
    local res = {}
    for k, v in pairs(a) do
        res[k] = merge_impl(v, b[k])
    end
    for k, v in pairs(b) do
        if a[k] == nil then
            res[k] = v
        end
    end
    return res
end

-- Merge two values.
--
-- Prefer the latter.
--
-- TODO: Don't merge arrays.
function methods.merge(_self, a, b)
    return merge_impl(a, b)
end

local function map_impl(schema, data, f, ctx)
    if is_scalar(schema) then
        -- TODO: Support a scalar within an array.
        local w = {
            path = ctx.path,
            error = function(message, ...)
                walkthrough_error(ctx, message, ...)
            end,
        }
        return f(schema, data, w, ctx.f_ctx)
    elseif schema.type == 'record' then
        if data ~= nil and type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a record: %q',
                type(data))
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

        if type(data) ~= 'table' then
            walkthrough_error(ctx, 'Unexpected data type for a map: %q',
                type(data))
        end

        local res = {}
        for field_name, field_value in pairs(data) do
            walkthrough_enter(ctx, field_name)
            local new_field_name = map_impl(schema.key, field_name, f, ctx)
            local new_field_value = map_impl(schema.value, field_value, f, ctx)
            res[new_field_name] = new_field_value
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

-- schema.record({
--     foo = schema.scalar(<...>),
-- }, {
--     <..annotations..>
-- })
local function record(fields, annotations)
    return {
        type = 'record',
        fields = fields or {},
        annotations = annotations or {},
    }
end

-- schema.scalar({
--     type = 'string',
--     my_annotation = <...>,
-- })
local function scalar(scalar_def)
    assert(scalar_def.type ~= nil)
    return scalar_def
end

-- Create a record, which is composition of fields from two records.
local function mix(a, b)
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
    for k, v in pairs(a.annotations) do
        annotations[k] = v
    end

    for field_name, field_def in pairs(b.fields) do
        if fields[field_name] ~= nil then
            -- XXX: Proper error reporting.
            error('XXX')
        end
        fields[field_name] = field_def
    end
    for k, v in pairs(b.annotations) do
        if annotations[k] ~= nil then
            -- XXX: Proper error reporting.
            error('XXX')
        end
        annotations[k] = v
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

local function map(map_def)
    assert(map_def.key ~= nil)
    assert(map_def.value ~= nil)
    return {
        type = 'map',
        key = map_def.key,
        value = map_def.value,
    }
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

local function new(name, schema, opts)
    opts = opts or {}
    local instance_methods = opts.methods or {}

    return setmetatable({
        name = name,
        schema = schema,
        methods = instance_methods,
    }, schema_mt)
end

-- }}} Module functions

return {
    -- TODO: add .enum()
    record = record,
    scalar = scalar,
    -- TODO: add .array()
    mix = mix,
    annotate = annotate,
    map = map,
    union_of_records = union_of_records,
    new = new,
}
