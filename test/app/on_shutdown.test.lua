env = require('test_run')
test_run = env.new()
build_dir = os.getenv("BUILDDIR") .. "/test/app/"
soext = (jit.os == "OSX" and "dylib" or "so")
on_shutdownlib = "on_shutdownlib."..soext
os.execute(string.format("cp %s/%s .", build_dir, on_shutdownlib))

test_run:cmd('create server test with script="app/on_shutdown.lua"')
test_run:cmd('start server test')
test_run:cmd('switch test')

module = require('on_shutdownlib')
module.cfg{}

test_run:cmd('switch default')
test_run:cmd('stop server test')
os.execute(string.format("rm %s", on_shutdownlib))
os.execute(string.format("grep -r \"on_shutdown_module_stop_func\" on_shutdown.log"))
os.execute(string.format("grep -r \"on_shutdown_module_join_func start\" on_shutdown.log"))
os.execute(string.format("grep -r \"module_fiber_f finished\" on_shutdown.log"))
os.execute(string.format("grep -r \"on_shutdown_module_join_func finished\" on_shutdown.log"))
test_run:cmd('cleanup server test')
test_run:cmd('delete server test')
