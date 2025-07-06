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
	core.info("lua received core for " .. ucore:comm() .. "[pid=" ..
	    attrs["pid"] .. "] at " .. ucore:path())

	for _, rule in ipairs(rules) do
		if rule:match(ucore) then
			return rule:apply(ucore)
		end
	end

	return true
end
