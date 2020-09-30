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

-- They all are sitting in cfunc.so. The "nop" module
-- contains functions which are simply return nothing.
old_module = cmod.load('cfunc')
old_cfunc_nop = old_module:load('cfunc_nop')
old_cfunc_fetch_evens = old_module:load('cfunc_fetch_evens')
old_cfunc_multireturn = old_module:load('cfunc_multireturn')
old_cfunc_args = old_module:load('cfunc_args')
old_cfunc_sum = old_module:load('cfunc_sum')
-- Test for error on nonexisting function.
_, err = pcall(old_module.load, old_module, 'no-such-func')
assert(err ~= nil)

-- Make sure they all are callable.
old_cfunc_nop()
old_cfunc_fetch_evens()
old_cfunc_multireturn()
old_cfunc_args()
old_cfunc_sum()

-- Unload the module but keep old functions alive, so
-- they keep reference to NOP module internally
-- and still callable.
old_module:unload()
old_cfunc_nop()
old_cfunc_fetch_evens()
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
old_cfunc_fetch_evens:unload()
old_cfunc_multireturn:unload()
old_cfunc_args:unload()
old_cfunc_sum:unload()

-- And reload old module again.
old_module = cmod.load('cfunc')
assert(old_module.state == "cached")
old_cfunc_nop = old_module:load('cfunc_nop')

-- Overwrite module with new contents.
_ = pcall(fio.unlink(cfunc_path))
fio.symlink(cfunc2_path, cfunc_path)

-- Now try to load a function from old module,
-- we should get chache invalidation but all
-- old functions should be callable.
new_module = cmod.load('cfunc')
assert(old_module.state == "orphan")

-- Still all functions from old module should
-- be callable.
old_cfunc_nop = old_module:load('cfunc_nop')
old_cfunc_fetch_evens = old_module:load('cfunc_fetch_evens')
old_cfunc_multireturn = old_module:load('cfunc_multireturn')
old_cfunc_args = old_module:load('cfunc_args')
old_cfunc_sum = old_module:load('cfunc_sum')

-- New module is loaded, lets lookup for updated symbols.
new_cfunc_nop = new_module:load('cfunc_nop')
new_cfunc_fetch_evens = new_module:load('cfunc_fetch_evens')
new_cfunc_multireturn = new_module:load('cfunc_multireturn')
new_cfunc_args = new_module:load('cfunc_args')
new_cfunc_sum = new_module:load('cfunc_sum')

-- Call old functions.
old_cfunc_nop()
old_cfunc_fetch_evens()
old_cfunc_multireturn()
old_cfunc_args()
old_cfunc_sum()

-- And newly loaded ones.
new_cfunc_nop()
new_cfunc_multireturn()
new_cfunc_fetch_evens({2,4,6})
new_cfunc_fetch_evens({1,2,3})  -- error
new_cfunc_args(1, "hello")
new_cfunc_sum(1) -- error
new_cfunc_sum(1,2)

-- Cleanup old module's functions.
old_cfunc_nop:unload()
old_cfunc_fetch_evens:unload()
old_cfunc_multireturn:unload()
old_cfunc_args:unload()
old_cfunc_sum:unload()

-- Cleanup new module data.
new_cfunc_nop:unload()
new_cfunc_multireturn:unload()
new_cfunc_fetch_evens:unload()
new_cfunc_args:unload()
new_cfunc_sum:unload()
new_module:unload()

--
-- Cleanup the generated symlink
_ = pcall(fio.unlink(cfunc_path))
