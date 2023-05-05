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

    for username, setting in pairs(credentials.users or {}) do
        if box.schema.user.exists(username) then
            log.debug('credentials.apply: user %q already exists', username)
            goto continue
        end
        local password
        if setting ~= nil and setting.passwd ~= nil then
            password = setting.passwd.plain
        end
        box.schema.user.create(username, {password = password})
        for _, grant in ipairs(setting.grant or {}) do
            box.schema.user.grant(username, grant)
        end
        ::continue::
    end
end

return {
    name = 'credentials',
    apply = apply
}
