test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
replica_set = require('fast_replica')

--
-- gh-3443: Check that changes done to spaces marked as local
-- are not replicated, but vclock is still promoted.
--

s1 = box.schema.space.create('test1', {engine = engine})
_ = s1:create_index('pk')
s2 = box.schema.space.create('test2', {engine = engine, is_local = true})
_ = s2:create_index('pk')
s1.is_local
s2.is_local

-- Check is_local to group_id mapping.
box.space._space:get(s1.id)[6]
box.space._space:get(s2.id)[6]

-- Check that group_id is immutable.
box.space._space:update(s1.id, {{'=', 6, {group_id = 1}}}) -- error
box.space._space:update(s2.id, {{'=', 6, {group_id = 0}}}) -- error

-- Currently, there are only two replication groups:
-- 0 (global) and 1 (local)
box.space._space:insert{9000, 1, 'test', engine, 0, {group_id = 2}, {}} -- error

-- Temporary local spaces should behave in the same fashion as
-- plain temporary spaces, i.e. neither replicated nor persisted.
s3 = box.schema.space.create('test3', {is_local = true, temporary = true})
_ = s3:create_index('pk')
s3.is_local
s3.temporary

_ = s1:insert{1}
_ = s2:insert{1}
_ = s3:insert{1}
box.snapshot()
_ = s1:insert{2}
_ = s2:insert{2}
_ = s3:insert{2}

box.schema.user.grant('guest', 'replication')
replica_set.create(test_run, 'local_spaces')
test_run:cmd("start server local_spaces")

test_run:cmd("switch local_spaces")
box.space.test1.is_local
box.space.test2.is_local
box.space.test3.is_local
box.space.test3.temporary
box.space.test1:select()
box.space.test2:select()
box.space.test3:select()
box.cfg{read_only = true} -- local spaces ignore read_only
for i = 1, 3 do box.space.test2:insert{i, i} end
for i = 1, 3 do box.space.test3:insert{i, i, i} end

test_run:cmd("switch default")
_ = s1:insert{3}
_ = s2:insert{3}
_ = s3:insert{3}
vclock = test_run:get_vclock('default')
_ = test_run:wait_vclock('local_spaces', vclock)

test_run:cmd("switch local_spaces")
box.space.test1:select()
box.space.test2:select()
box.space.test3:select()
test_run:cmd("restart server local_spaces")
box.space.test1:select()
box.space.test2:select()
box.space.test3:select()

test_run:cmd("switch default")
replica_set.drop(test_run, 'local_spaces')
test_run:cleanup_cluster()
box.schema.user.revoke('guest', 'replication')

s1:select()
s2:select()
s3:select()

s1:drop()
s2:drop()
s3:drop()
