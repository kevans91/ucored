--
-- Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
--
-- SPDX-License-Identifier: BSD-2-Clause
--

local core = require('core')
local config = require('config')

local rules = config.load()

return function(ucore)
	local attrs = ucore:attributes()
	local comm = ucore:comm() or "unknown"
	local path = ucore:path()
	local pwd = ucore:pwd()

	local msg = "lua received core for " .. comm .. "[pid=" .. attrs["pid"] .. "]"
	if path then
		msg = msg .. " at " .. path
	else
		msg = msg .. " via /dev/ucore"
	end

	if pwd then
		msg = msg .. " with pwd " .. pwd
	end
	core.info(msg)

	for _, rule in ipairs(rules) do
		if rule:match(ucore) then
			return rule:apply(ucore)
		end
	end

	return true
end
