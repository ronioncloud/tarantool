-- Script for box-tap/errinj_set_with_enviroment_vars.test.lua test.

local tap = require('tap')
local errinj = box.error.injection

local res = tap.test('set errinjs via environment variables', function(test)
    test:plan(3)
    test:is(errinj.get('ERRINJ_TESTING'), true, "set bool error injection")
    test:is(errinj.get('ERRINJ_WAL_WRITE_PARTIAL'), 1, "set int error injection")
    test:is(errinj.get('ERRINJ_VY_READ_PAGE_TIMEOUT'), 2.5, "set double error injection")
end)

os.exit(res and 0 or 1)
