local log = require('conf.utils.log')

local function grant_permissions(name, privileges, grant_f)
    for _, privilege in ipairs(privileges or {}) do
        for _, permission in ipairs(privilege.permissions or {}) do
            log.verbose('credentials.apply: grant %s to %s (if not exists)',
                permission, name)
            local opts = {if_not_exists = true}
            if privilege.universe then
                grant_f(name, permission, 'universe', nil, opts)
            end
            for _, space in ipairs(privileges.spaces or {}) do
                grant_f(name, permission, 'space', space, opts)
            end
            for _, func in ipairs(privileges.functions or {}) do
                grant_f(name, permission, 'function', func, opts)
            end
            for _, seq in ipairs(privileges.sequences or {}) do
                grant_f(name, permission, 'sequence', seq, opts)
            end
        end
    end
end

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

    -- Creare roles and grant then permissions. Skip assigning
    -- underlying roles till all the roles will be created.
    for rolename, role_def in pairs(credentials.roles or {}) do
        if box.schema.role.exists(rolename) then
            log.verbose('credentials.apply: role %q already exists', rolename)
        else
            box.schema.role.create(rolename)
            if role_def ~= nil then
                grant_permissions(rolename, role_def.privileges,
                    box.schema.role.grant)
            end
        end
    end

    -- Assign underlying roles.
    for rolename, role_def in pairs(credentials.roles or {}) do
        for _, role in ipairs(role_def.roles or {}) do
            log.verbose('credentials.apply: add role %q as underlying for ' ..
                'role %q (if not exists)', role, rolename)
            box.schema.role.grant(rolename, role, nil, nil, {if_not_exists = true})
        end
    end

    -- Create users, set them passwords, assign roles, grant
    -- permissions.
    for username, user_def in pairs(credentials.users or {}) do
        -- Create a user.
        if box.schema.user.exists(username) then
            log.verbose('credentials.apply: user %q already exists', username)
        else
            log.verbose('credentials.apply: create user %q', username)
            box.schema.user.create(username)
        end

        -- Set a password.
        if user_def == nil or user_def.password == nil or
                next(user_def.password) == nil then
            if username ~= 'guest' then
                log.verbose('credentials.apply: remove password for user %q',
                    username)
                -- TODO: Check for hashes and if absent remove the password.
            end
        elseif user_def ~= nil and user_def.password ~= nil and
                user_def.password.plain ~= nil then
            if username == 'guest' then
                error('Setting a password for the guest user has no effect')
            end
            -- TODO: Check if the password can be hashed in somewhere other then
            --       'chap-sha1' or if the select{username} may return table of
            --       a different shape.
            local stored_user_def = box.space._user.index.name:get({username})
            local stored_hash = stored_user_def[5]['chap-sha1']
            local given_hash = box.schema.user.password(user_def.password.plain)
            if given_hash == stored_hash then
                log.verbose('credentials.apply: a password is already set ' ..
                    'for user %q', username)
            else
                log.verbose('credentials.apply: set a password for user %q',
                    username)
                box.schema.user.passwd(username, user_def.password.plain)
            end
        --[[
        elseif sha1() then
        elseif sha256() then
        ]]--
        else
            assert(false)
        end

        -- Assign roles and grant permssions.
        if user_def ~= nil then
            for _, role in ipairs(user_def.roles or {}) do
                log.verbose('grant role %q to user %q (if not exists)', role,
                    username)
                box.schema.user.grant(username, role, nil, nil,
                    {if_not_exists = true})
            end

            grant_permissions(username, user_def.privileges,
                box.schema.user.grant)
        end
    end
end

return {
    name = 'credentials',
    apply = apply
}
