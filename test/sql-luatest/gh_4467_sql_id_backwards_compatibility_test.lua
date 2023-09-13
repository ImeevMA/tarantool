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

g.test_transaction_statements = function()
    g.server:exec(function()
        local exp = {row_count = 0}
        t.assert_equals(box.execute([[START TRANSACTION;]]), exp)
        t.assert_equals(box.execute([[SAVEPOINT ASD;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK TO Asd;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK TO "ASD";]]), exp)
        local res, err = box.execute([[ROLLBACK TO "aSd";]])
        t.assert_equals(err.message, "Can not rollback to savepoint: "..
                        "the savepoint does not exist")
        t.assert_equals(box.execute([[RELEASE asD;]]), exp)
        t.assert_equals(box.execute([[ROLLBACK;]]), exp)
    end)
end
