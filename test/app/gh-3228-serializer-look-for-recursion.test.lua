test_run = require('test_run').new()

--
-- gh-3228: Check the error message in the case of a __serialize
-- function generating infinite recursion.
--
setmetatable({}, {__serialize = function(a) return a end})
setmetatable({}, {__serialize = function(a) return {a} end})
setmetatable({}, {__serialize = function(a) return {a, a} end})
setmetatable({}, {__serialize = function(a) return {a, a, a} end})
setmetatable({}, {__serialize = function(a) return {a, 1} end})
setmetatable({}, {__serialize = function(a) return {{{{a}}}} end})
setmetatable({}, {__serialize = function(a) return {{{{1}}}} end})
b = {}
setmetatable({b}, {__serialize = function(a) return {a_1 = a, a_2 = a, b_1 = b, b_2 = b} end})
setmetatable({b}, {__serialize = function(a) return {a_1 = a, a_2 = {a, b}, b = b} end})

--
--Check that __eq metamethod is ignored.
--
local table = setmetatable({}, {__eq = function(a, b) error('__eq is called') end})
setmetatable(table, {__serialize = function(a) return a end})
