local log = require('conf.utils.log')

local function grant_permissions(name, privileges, box_schema)
    for _, privilege in ipairs(privileges or {}) do
        for _, permission in ipairs(privilege.permissions or {}) do
            log.verbose('credentials.apply: grant %s to %s', permission, name)
            if privilege.universe then
                box_schema.grant(name, permission, 'universe', nil, {if_not_exists = true})
                goto continue
            end
            for _, space in ipairs(privileges.spaces or {}) do
                box_schema.grant(name, permission, 'space', space, {if_not_exists = true})
            end
            for _, func in ipairs(privileges.functions or {}) do
                box_schema.grant(name, permission, 'function', func, {if_not_exists = true})
            end
            for _, seq in ipairs(privileges.sequences or {}) do
                box_schema.grant(name, permission, 'sequence', seq, {if_not_exists = true})
            end
            ::continue::
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

    for rolename, role_def in pairs(credentials.roles or {}) do
        if box.schema.role.exists(rolename) then
            log.verbose('credentials.apply: role %q already exists', rolename)
            goto continue
        end
        box.schema.role.create(rolename)
        if role_def == nil then
            log.verbose('credentials.apply: role %q doesn\'t ' ..
                        'have any privileges', rolename)
        else
            grant_permissions(rolename, role_def.privileges, box.schema.role)
        end
        ::continue::
    end

    -- The second run is required, so the order of role creation wouldn't
    -- break the role assignment.
    for rolename, role_def in pairs(credentials.roles or {}) do
        for _, role in ipairs(role_def.roles or {}) do
            box.schema.role.grant(rolename, role, nil, nil, {if_not_exists = true})
        end
    end

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
            -- TODO: check for hashes and if absent remove the password.
        elseif user_def ~= nil and user_def.password ~= nil and
                user_def.password.plain ~= nil then
            if username == 'guest' then
                error('Setting a password for the guest user has no effect')
            end
            -- TODO: check if the password can be hashed in somewhere other then 
            --       'chap-sha1' or if the select{username} may return table of
            --       a different shape.
            if (box.schema.user.password(user_def.password.plain) ~=
                    box.space._user.index.name:select{username}[1][5]['chap-sha1']) then
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

        if user_def == nil then
            log.verbose('credentials.apply: user %q doesn\'t ' ..
                        'have any privileges', username)
            goto continue
        end

        for _, role in ipairs(user_def.roles or {}) do
            log.verbose('grant role %q to user %q (if not exists)', role,
                username)
            box.schema.user.grant(username, role, nil, nil,
                {if_not_exists = true})
        end

        grant_permissions(username, user_def.privileges, box.schema.user)

        ::continue::
    end
end

return {
    name = 'credentials',
    apply = apply
}
