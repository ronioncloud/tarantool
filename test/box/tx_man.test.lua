env = require('test_run')
test_run = env.new()
test_run:cmd("create server tx_man with script='box/tx_man.lua'")
test_run:cmd("start server tx_man")
test_run:cmd("switch tx_man")

txn_proxy = require('txn_proxy')

s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s2 = box.schema.space.create('test2')
i21 = s2:create_index('pk', {parts={{1, 'uint'}}})
i22 = s2:create_index('sec', {parts={{2, 'uint'}}})

tx1 = txn_proxy.new()
tx2 = txn_proxy.new()
tx3 = txn_proxy.new()

-- Simple read/write conflicts.
s:replace{1, 0}
tx1:begin()
tx2:begin()
tx1('s:select{1}')
tx2('s:select{1}')
tx1('s:replace{1, 1}')
tx2('s:replace{1, 2}')
tx1:commit()
tx2:commit()
s:select{}

-- Simple read/write conflicts, different order.
s:replace{1, 0}
tx1:begin()
tx2:begin()
tx1('s:select{1}')
tx2('s:select{1}')
tx1('s:replace{1, 1}')
tx2('s:replace{1, 2}')
tx2:commit() -- note that tx2 commits first.
tx1:commit()
s:select{}

-- Implicit read/write conflicts.
s:replace{1, 0}
tx1:begin()
tx2:begin()
tx1("s:update({1}, {{'+', 2, 3}})")
tx2("s:update({1}, {{'+', 2, 5}})")
tx1:commit()
tx2:commit()
s:select{}

-- Implicit read/write conflicts, different order.
s:replace{1, 0}
tx1:begin()
tx2:begin()
tx1("s:update({1}, {{'+', 2, 3}})")
tx2("s:update({1}, {{'+', 2, 5}})")
tx2:commit() -- note that tx2 commits first.
tx1:commit()
s:select{}
s:delete{1}

-- Conflict in secondary index.
tx1:begin()
tx2:begin()
tx1("s:replace{1, 1}")
tx2("s:replace{2, 1}")
tx1:commit()
tx2:commit()
s:select{}
s:delete{1}

-- Conflict in secondary index, different order.
tx1:begin()
tx2:begin()
tx1("s:replace{1, 2}")
tx2("s:replace{2, 2}")
tx2:commit() -- note that tx2 commits first.
tx1:commit()
s:select{}
s:delete{2}

-- TXN is send to read view.
s:replace{1, 1}
s:replace{2, 2}
s:replace{3, 3}
tx1:begin()
tx2:begin()

tx1("s:select{}")
tx2("s:replace{1, 11}")
tx2("s:replace{2, 12}")
tx2:commit()
tx1("s:select{}")
tx1:commit()

s:delete{1}
s:delete{2}
s:delete{3}

-- TXN is send to read view but tries to replace and becomes conflicted.
s:replace{1, 1}
s:replace{2, 2}
s:replace{3, 3}
tx1:begin()
tx2:begin()

tx1("s:select{}")
tx2("s:replace{1, 11}")
tx2("s:replace{2, 12}")
tx2:commit()
tx1("s:select{}")
tx1("s:replace{3, 13}")
tx1("s:select{}")
tx1:commit()

s:delete{1}
s:delete{2}
s:delete{3}

-- Use two indexes
s:replace{1, 3}
s:replace{2, 2}
s:replace{3, 1}

tx1:begin()
tx2:begin()
tx1("i2:select{}")
tx2("i2:select{}")
tx1("s:replace{2, 4}")
tx1("i2:select{}")
tx2("i2:select{}")
tx1("s:delete{1}")
tx1("i2:select{}")
tx2("i2:select{}")
tx1:commit()
tx2("i2:select{}")
tx2:commit()
i2:select{}

s:delete{2}
s:delete{3}

-- More than two spaces
s:replace{1, 1}
s:replace{2, 2}
s2:replace{1, 2}
s2:replace{2, 1}
tx1:begin()
tx2:begin()
tx1("s:replace{3, 3}")
tx2("s2:replace{4, 4}")
tx1("s:select{}")
tx1("s2:select{}")
tx2("s:select{}")
tx2("s2:select{}")
tx1:commit()
tx2:commit()
s:select{}
s2:select{}
s:truncate()
s2:truncate()

-- Rollback
s:replace{1, 1}
s:replace{2, 2}
tx1:begin()
tx2:begin()
tx1("s:replace{4, 4}")
tx1("s:replace{1, 3}")
tx2("s:replace{3, 3}")
tx2("s:replace{1, 4}")
tx1("s:select{}")
tx2("s:select{}")
tx1:rollback()
tx2:commit()
s:select{}
s:truncate()

-- Delete the same value
s:replace{1, 1}
s:replace{2, 2}
s:replace{3, 3}
tx1:begin()
tx2:begin()
tx1("s:delete{2}")
tx1("s:select{}")
tx2("s:select{}")
tx2("s:delete{2}")
tx1("s:select{}")
tx2("s:select{}")
tx1:commit()
tx2:commit()
s:select{}
s:truncate()

-- Delete and rollback the same value
s:replace{1, 1}
s:replace{2, 2}
s:replace{3, 3}
tx1:begin()
tx2:begin()
tx1("s:delete{2}")
tx1("s:select{}")
tx2("s:select{}")
tx2("s:delete{2}")
tx1("s:select{}")
tx2("s:select{}")
tx1:rollback()
tx2:commit()
s:select{}
s:truncate()

-- Stack of replacements
tx1:begin()
tx2:begin()
tx3:begin()
tx1("s:replace{1, 1}")
tx1("s:select{}")
s:select{}
tx2("s:replace{1, 2}")
tx1("s:select{}")
s:select{}
tx3("s:replace{1, 3}")
s:select{}
tx1("s:select{}")
tx2("s:select{}")
tx3("s:select{}")
tx1:commit()
s:select{}
tx2:commit()
s:select{}
tx3:commit()
s:select{}

s:drop()
s2:drop()

-- https://github.com/tarantool/tarantool/issues/5423
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 0}
s:delete{1}
collectgarbage()
s:replace{1, 1}
s:replace{1, 2 }

s:drop()

-- https://github.com/tarantool/tarantool/issues/5628
s = box.schema.space.create('test')
i = s:create_index('pk', {parts={{1, 'uint'}}})

s:replace{1, 0}
s:delete{1}
tx1:begin()
tx1("s:replace{1, 1}")
s:select{}
tx1:commit()
s:select{}

s:drop()

s = box.schema.space.create('test')
i = s:create_index('pk', {parts={{1, 'uint'}}})
s:replace{1, 0}

tx1:begin()
tx2:begin()
tx1('s:select{1}')
tx1('s:replace{1, 1}')
tx2('s:select{1}')
tx2('s:replace{1, 2}')
tx1:commit()
tx2:commit()
s:select{}

s:drop()

s = box.schema.space.create('test')
i = s:create_index('pk')
s:replace{1}
collectgarbage('collect')
s:drop()

-- Fixme: anti-dependency
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

tx1:begin()
tx2:begin()
tx1('s:select({0}, {iterator=\'GT\'})')
tx2('s:select({0}, {iterator=\'GT\'})')
tx1("s:replace{1, 11}")
tx2("s:replace{2, 22}")
tx1:commit()
tx2:commit()

s:select{}

s:drop()

-- Fixme: write skew, initially empty space, select the whole space
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

tx1:begin()
tx2:begin()
tx1("s:select{}")
tx2("s:select{}")
tx1("s:replace{1, 11}")
tx2("s:replace{2, 22}")
tx1:commit()
tx2:commit()

s:select{}

s:drop()

-- G0
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:replace{1, 11}")
tx2("s:replace{1, 12}")
tx1("s:replace{2, 21}")
tx1:commit()
--tx1("s:select{1}")
s:select{}
tx2("s:replace{2, 22}")
tx2:commit()

s:select{}

s:drop()

-- Fixme: G1a (crash)
-- NB: now crashes only after running preceding testcases (isolated test passes)
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:replace{1, 101}")
tx2("s:select{}")
tx1:rollback()
tx2("s:select{}")
tx2:commit()

s:select{}

s:drop()

-- Fixme: Collect garbage (assertion !tuple->is_dirty in tuple_unref)
s = box.schema.space.create('test')
i1 = s:create_index('pk')
s:replace{1}
s:drop()
collectgarbage('collect')

-- Creating unique index, then commit non-unique tuples
s = box.schema.space.create('test')
i1 = s:create_index('pk')
tx1:begin()
dd = s:create_index('dd', {unique = false, parts = {{2, 'uint'}}})
tx1("s:replace{1, 11}")
tx1("s:replace{2, 11}")
i2 = s:create_index('sk', {unique = true, parts = {{2, 'uint'}}})
tx1:commit()

s:select{}
i2:select{}

s:drop()

-- G1b
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:replace{1, 101}")
tx2("s:select{}")
tx1("s:replace{1, 11}")
tx1:commit()
tx2("s:select{}")
tx2:commit()

s:select{}

s:drop()

-- G1с
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:replace{1, 11}")
tx2("s:replace{2, 22}")
tx1("s:select{}")
tx2("s:select{}")
tx1:commit()
tx2:commit()

s:select{}

s:drop()

-- otv
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx3:begin()
tx1("s:replace{1, 11}")
tx1("s:replace{2, 19}")
tx2("s:replace{1, 12}")
tx1:commit()
tx3("s:select{}")
tx2("s:replace{2, 18}")
tx3("s:select{}")
tx2:commit()
tx3:commit()

s:select{}

s:drop()

-- Fixme: pmp
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
-- nothing
tx1("i2:select{30}")
tx2("s:insert{3, 30}")
tx2:commit()
-- again nothing
tx1("i2:select{30}")
tx1:commit()

s:select{}

s:drop()

-- Fixme: pmp-write
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:update({2}, {{'+', 2, 10}})")
tx1("s:update({1}, {{'+', 2, 10}})")
tx2("s:select{}")
tx2("i2:delete{20}")
tx1:commit()
tx2("s:select{}")
tx2:commit()
--tx1("s:select{}")

s:select{}

s:drop()

-- p4
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:select{1}")
tx2("s:select{1}")
tx1("s:replace{1, 11}")
tx2("s:replace{1, 11}")
tx1:commit()
tx2:commit()

s:select{}

s:drop()

-- g-single
s = box.schema.space.create('test')
i1 = s:create_index('pk', {parts={{1, 'uint'}}})
i2 = s:create_index('sec', {parts={{2, 'uint'}}})

s:replace{1, 10}
s:replace{2, 20}

tx1:begin()
tx2:begin()
tx1("s:select{1}")
tx2("s:select{1}")
tx2("s:select{2}")
tx2("s:replace{1, 12}")
tx2("s:replace{2, 18}")
tx2:commit()
tx1("s:select{2}")
tx1:commit()

s:select{}

s:drop()

-- Fixme: crash on duplicate when rollback
s = box.schema.space.create('test')
i = s:create_index('pk', {parts={{1, 'uint'}}})

s:replace{1, 0}
s:delete{1}
tx2:begin()
tx2("s:replace{1, 1}")
tx2("s:delete{1}")
tx1:begin()
tx1("s:replace{1, 1}")
s:select{}
tx1:commit()
tx2:commit()
s:select{}

s:drop()

test_run:cmd("switch default")
test_run:cmd("stop server tx_man")
test_run:cmd("cleanup server tx_man")
