local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function()
    g.server = server:new({alias = 'master'})
    g.server:start()
end)

g.after_all(function()
    g.server:stop()
end)

--
-- Make sure that only TRUE, FALSE and a string literal are accepted as
-- a value of a session setting.
--
g.test_set_session_value = function()
    g.server:exec(function()
        local _, err = box.execute([[SET SESSION "sql_seq_scan" = false;]])
        t.assert_equals(err, nil)
        t.assert_equals(box.space._session_settings:get('sql_seq_scan'),
                        {'sql_seq_scan', false})
        _, err = box.execute([[SET SESSION "sql_seq_scan" = TRUE;]])
        t.assert_equals(err, nil)
        t.assert_equals(box.space._session_settings:get('sql_seq_scan'),
                        {'sql_seq_scan', true})

        _, err = box.execute([[SET SESSION "sql_default_engine" = 'vinyl';]])
        t.assert_equals(err, nil)
        t.assert_equals(box.space._session_settings:get('sql_default_engine'),
                        {'sql_default_engine', 'vinyl'})
        _, err = box.execute([[SET SESSION "sql_default_engine" = 'memtx';]])
        t.assert_equals(err, nil)

        for _, value in pairs({'NULL', 'UNKNOWN', '1', '1.5', '1e0',
                               "X'00'", '?', '(true)'}) do
            local sql = [[SET SESSION "sql_seq_scan" = ]] .. value .. ';'
            _, err = box.execute(sql)
            t.assert_str_contains(err.message, 'Syntax error', false, sql)
        end

        local exp = 'Session setting sql_seq_scan expected a value of type ' ..
                    'boolean'
        _, err = box.execute([[SET SESSION "sql_seq_scan" = 'true';]])
        t.assert_equals(err.message, exp)

        exp = 'Space engine \'it\'s\' does not exist'
        _, err = box.execute([[SET SESSION "sql_default_engine" = 'it''s';]])
        t.assert_equals(err.message, exp)
    end)
end
