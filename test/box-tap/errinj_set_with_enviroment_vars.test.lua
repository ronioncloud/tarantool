#!/usr/bin/env tarantool
local fio = require('fio')

-- Execute errinj_set_with_enviroment_vars_script.lua
-- via tarantool with presetted environment variables.
local TARANTOOL_PATH = arg[-1]
local set_env_str = 'ERRINJ_TESTING=true ERRINJ_WAL_WRITE_PARTIAL=1 ERRINJ_VY_READ_PAGE_TIMEOUT=2.5'
local path_to_test_file = fio.pathjoin(
        os.getenv('PWD'),
        'box-tap',
        'errinj_set_with_enviroment_vars_script.lua')
local shell_command = ('%s %s %s'):format(set_env_str, TARANTOOL_PATH, path_to_test_file)

os.exit(os.execute(shell_command))
