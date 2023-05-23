local log = require('conf.utils.log')

local function apply(configdata)
    local credentials = configdata:get('credentials')
    if credentials == nil then
        return
    end

    -- TODO: What if all the instances in a replicaset are
    -- read-only at configuration applying? They all will ignore
    -- the section and so it will never be applied -- even when
    -- some instance goes to RW.
    --
    -- Moreover, this skip is silent: no log warnings or issue
    -- reporting.
    --
    -- OTOH, a replica (downstream) should ignore all the config
    -- data that is persisted.
    --
    -- A solution could be postpone applying of such data till
    -- RW state. The applying should check that the data is not
    -- added/updated already (arrived from master).
    if box.info.ro then
        log.verbose('credentials.apply: skip the credentials section, ' ..
            'because the instance is in the read-only mode')
        return
    end

    for rolename, grants in pairs(credentials.roles or {}) do
        if box.schema.role.exists(rolename) then
            log.debug('credentials.apply: role %q already exists', rolename)
            goto continue
        end
        box.schema.role.create(rolename)
        for _, privilege in ipairs(grants.privileges or {}) do
            if grants.universe then
                box.schema.role.grant(rolename, privilege, 'universe')
                break
            end
            for _, space in ipairs(grants.spaces or {}) do
                box.schema.role.grant(rolename, privilege, 'space', space)
            end
            for _, func in ipairs(grants.functions or {}) do
                box.schema.role.grant(rolename, privilege, 'function', func)
            end
            for _, seq in ipairs(grants.sequences or {}) do
                box.schema.role.grant(rolename, privilege, 'sequence', seq)
            end
            for _, role in ipairs(grants.roles or {}) do
                box.schema.role.grant(rolename, role)
            end
        end
        ::continue::
    end

    -- TODO: Remove users that are not listed in the
    -- configuration.

    for username, user_def in pairs(credentials.users or {}) do
        if box.schema.user.exists(username) then
            log.verbose('credentials.apply: user %q already exists', username)
        else
            log.verbose('credentials.apply: create user %q', username)
            box.schema.user.create(username)
        end

        if user_def == nil or user_def.password == nil or
                next(user_def.password) == nil then
            log.verbose('credentials.apply: remove password for user %q', username)
            -- TODO: Remove the password.
        elseif user_def ~= nil and user_def.password ~= nil and
                user_def.password.plain ~= nil then
            if username == 'guest' then
                error('Setting a password for the guest user has no effect')
            end
            -- TODO: Should we skip this step if the password already set to
            -- this value? I think that there is nothing good in redundant DML
            -- operations.
            log.verbose('credentials.apply: set a password for user %q',
                username)
            box.schema.user.passwd(username, user_def.password.plain)
        --[[
        elseif sha1() then
        elseif sha256() then
        ]]--
        else
            assert(false)
        end

        -- TODO: Remove roles that are not listed in the
        -- configuration.

        for _, role in ipairs(user_def.roles or {}) do
            log.verbose('grant role %q to user %q (if not exists)', role,
                username)
            box.schema.user.grant(username, role, nil, nil,
                {if_not_exists = true})
        end

        -- TODO: Grant privileges.
    end
end

return {
    name = 'credentials',
    apply = apply
}
