--
-- gh-4642: New cmod module to be able to
-- run C stored functions on read only nodes
-- without requirement to register them with
-- box.schema.func help.
--
build_path = os.getenv("BUILDDIR")
package.cpath = build_path..'/test/box/?.so;'..build_path..'/test/box/?.dylib;'..package.cpath

cmod = require('cmod')
fio = require('fio')

ext = (jit.os == "OSX" and "dylib" or "so")

cfunc_path = fio.pathjoin(build_path, "test/box/cfunc.") .. ext
cfunc1_path = fio.pathjoin(build_path, "test/box/cfunc1.") .. ext
cfunc2_path = fio.pathjoin(build_path, "test/box/cfunc2.") .. ext

_ = pcall(fio.unlink(cfunc_path))
fio.symlink(cfunc1_path, cfunc_path)

_, err = pcall(cmod.load, 'non-such-module')
assert(err ~= nil)

-- All functions are sitting in cfunc.so.
old_module = cmod.load('cfunc')
assert(old_module['tt_dev.refs'] == 1)
old_module_copy = cmod.load('cfunc')
assert(old_module['tt_dev.refs'] == 2)
assert(old_module_copy['tt_dev.refs'] == 2)
old_module_copy:unload()
assert(old_module['tt_dev.refs'] == 1)
old_cfunc_nop = old_module:load('cfunc_nop')
old_cfunc_fetch_seq_evens = old_module:load('cfunc_fetch_seq_evens')
old_cfunc_multireturn = old_module:load('cfunc_multireturn')
old_cfunc_args = old_module:load('cfunc_args')
old_cfunc_sum = old_module:load('cfunc_sum')
assert(old_module['tt_dev.refs'] == 6)

-- Test for error on nonexisting function.
_, err = pcall(old_module.load, old_module, 'no-such-func')
assert(err ~= nil)

-- Make sure they all are callable.
old_cfunc_nop()
old_cfunc_fetch_seq_evens()
old_cfunc_multireturn()
old_cfunc_args()
old_cfunc_sum()

-- Unload the module but keep old functions alive, so
-- they keep reference to NOP module internally
-- and still callable.
old_module:unload()
-- Test refs via function name.
assert(old_cfunc_nop['tt_dev.cmod.refs'] == 5)
old_cfunc_nop()
old_cfunc_fetch_seq_evens()
old_cfunc_multireturn()
old_cfunc_args()
old_cfunc_sum()

-- The module is unloaded I should not be able
-- to load new shared library.
old_module:load('cfunc')
-- Neither I should be able to unload module twise.
old_module:unload()

-- Clean old functions.
old_cfunc_nop:unload()
old_cfunc_fetch_seq_evens:unload()
old_cfunc_multireturn:unload()
old_cfunc_args:unload()
assert(old_cfunc_sum['tt_dev.cmod.refs'] == 1)
old_cfunc_sum:unload()

-- And reload old module again.
old_module = cmod.load('cfunc')
old_module_id = old_module['tt_dev.id']
assert(old_module['tt_dev.refs'] == 1)

-- Overwrite module with new contents.
_ = pcall(fio.unlink(cfunc_path))
fio.symlink(cfunc2_path, cfunc_path)

-- Load new module, cache should be updated.
new_module = cmod.load('cfunc')
new_module_id = new_module['tt_dev.id']

-- Old and new module keep one reference with
-- different IDs.
assert(old_module['tt_dev.refs'] == 1)
assert(old_module['tt_dev.refs'] == new_module['tt_dev.refs'])
assert(old_module_id ~= new_module_id)

-- All functions from old module should be loadable.
old_cfunc_nop = old_module:load('cfunc_nop')
old_cfunc_fetch_seq_evens = old_module:load('cfunc_fetch_seq_evens')
old_cfunc_multireturn = old_module:load('cfunc_multireturn')
old_cfunc_args = old_module:load('cfunc_args')
old_cfunc_sum = old_module:load('cfunc_sum')
assert(old_cfunc_nop['tt_dev.cmod.id'] == old_module_id)
assert(old_cfunc_fetch_seq_evens['tt_dev.cmod.id'] == old_module_id)
assert(old_cfunc_multireturn['tt_dev.cmod.id'] == old_module_id)
assert(old_cfunc_args['tt_dev.cmod.id'] == old_module_id)
assert(old_cfunc_sum['tt_dev.cmod.id'] == old_module_id)
assert(old_module['tt_dev.refs'] == 6)

-- Lookup for updated symbols.
new_cfunc_nop = new_module:load('cfunc_nop')
new_cfunc_fetch_seq_evens = new_module:load('cfunc_fetch_seq_evens')
new_cfunc_multireturn = new_module:load('cfunc_multireturn')
new_cfunc_args = new_module:load('cfunc_args')
new_cfunc_sum = new_module:load('cfunc_sum')
assert(new_cfunc_nop['tt_dev.cmod.id'] == new_module_id)
assert(new_cfunc_fetch_seq_evens['tt_dev.cmod.id'] == new_module_id)
assert(new_cfunc_multireturn['tt_dev.cmod.id'] == new_module_id)
assert(new_cfunc_args['tt_dev.cmod.id'] == new_module_id)
assert(new_cfunc_sum['tt_dev.cmod.id'] == new_module_id)
assert(new_module['tt_dev.refs'] == 6)

-- Call old functions.
old_cfunc_nop()
old_cfunc_fetch_seq_evens()
old_cfunc_multireturn()
old_cfunc_args()
old_cfunc_sum()

-- Call new functions.
new_cfunc_nop()
new_cfunc_multireturn()
new_cfunc_fetch_seq_evens({2,4,6})
new_cfunc_fetch_seq_evens({1,2,3})  -- error, odd numbers sequence
new_cfunc_args(1, "hello")
new_cfunc_sum(1) -- error, one arg passed
new_cfunc_sum(1,2)

-- Cleanup old module's functions.
old_cfunc_nop:unload()
old_cfunc_fetch_seq_evens:unload()
old_cfunc_multireturn:unload()
old_cfunc_args:unload()
old_cfunc_sum:unload()
old_module:unload()

-- Cleanup new module data.
new_cfunc_nop:unload()
new_cfunc_multireturn:unload()
new_cfunc_fetch_seq_evens:unload()
new_cfunc_args:unload()
new_cfunc_sum:unload()
new_module:unload()

-- Cleanup the generated symlink
_ = pcall(fio.unlink(cfunc_path))
