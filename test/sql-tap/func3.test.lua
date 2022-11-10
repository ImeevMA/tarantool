#!/usr/bin/env tarantool
local test = require("sqltester")
test:plan(8)

--
-- gh-3929: sql: ANSI aliases for LENGTH().
--
local suits = {}
suits[1] = {str = '123456789', len = 9}
suits[2] = {str = '\x80', len = 1}
suits[3] = {str = '\x61\x62\x63', len = 3}
suits[4] = {str = '\x7f\x80\x81', len = 3}
suits[5] = {str = '\x61\xc0', len = 2}
suits[6] = {str = '\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80', len = 10}
suits[7] = {str = '\x80\x80\x80\x80\x80\xf0\x90\x80\x80\x80', len = 7}
suits[8] = {str = '\x80\x80\x80\x80\x80\xf0\x90\x80\x80\xff', len = 7}

for k,v in pairs(suits) do
    test:do_execsql_test(
        "func3-6."..k,
        "SELECT CHAR_LENGTH('"..v.str.."'), CHARACTER_LENGTH('"..v.str.."');",
        {v.len, v.len})
end

test:finish_test()
