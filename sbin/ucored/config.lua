--
-- Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
--
-- SPDX-License-Identifier: BSD-2-Clause
--

local config = {}
local core = require('core')

-- Assumes fullpath is actually a path to a file, which is the case with
-- ucore:path() at least.
local function filename_part(fullpath)
	if not fullpath:match("/") then
		return fullpath
	end

	return fullpath:match("/([^/]+)$")
end

local function replace_symbols(ucore, path)
	local attrs = ucore:attributes()
	local symbols = {
		["%j"] = attrs.jid,
		["%n"] = ucore:filename(),
		["%P"] = attrs.ppid,
		["%p"] = attrs.pid,
		["%s"] = attrs.signal,
	}

	for sym, val in pairs(symbols) do
		path = path:gsub("%" .. sym, tostring(val))
	end

	return path
end

local function process_destpath(ucore, path)
	local corepath = ucore:path()
	local corefile = ucore:filename()

	-- We replace the symbols in the leading bits of the path independently
	-- of the core filename part of the path
	path = replace_symbols(ucore, path)

	local dir, filename
	if not path:match("/") then
		-- No directories, effectively just a rename rule.
		filename = path
	elseif path:match("/$") then
		-- Obviously no filename here, the path ends in '/'
		dir = path
	elseif core.isdir(path) then
		-- No further augmentation needed here; the path is already
		-- a directory, so clearly we can just make the final filename
		goto nomk
	else
		filename = filename_part(path)
		dir = path:sub(1, #path - #filename - 1)
	end

	-- We only peeled apart hte dir part to create it; path should still be
	-- accurate since we replaced symbols up above.
	if dir then
		local is_relative = dir:sub(1, 1) ~= "/"

		-- Relative paths are relative to where the core was generated,
		-- not where ucored(8) is running.
		if is_relative then
			local coredir = corepath:sub(1, #corepath - #corefile - 1)

			dir = coredir .. "/" .. dir
		end

		assert(core.mkpath(dir))

		-- If it was relative, we need to reconstruct the path now that
		-- we've altered it by making it absolute above.
		if is_relative then
			if filename then
				path = dir .. filename
			else
				path = dir
			end
		end
	end

::nomk::
	-- If we had a filename part, then it's already been desymbolized and
	-- we'll just use it as-is.  Otherwise, we're expected to re-use the
	-- filename of the original core.
	if not filename then
		path = path .. "/" .. replace_symbols(ucore, corefile)
	end

	return path
end

local action_handlers = {
	discard = {
		apply = function(_, ucore)
			local path = ucore:path()
			local ok, err = os.remove(path)

			if not ok then
				core.error(path .. ": " .. err)
			end

			return ok
		end,
	},
	ignore = {
		apply = function()
			return true
		end,
	},
	move = {
		apply = function(action, ucore)
			local dest = process_destpath(ucore, action.destination)
			local ok, err = ucore:move(dest)

			if not ok then
				core.error(ucore:path() .. ": " .. err)
			end

			return ok
		end,
		validate = function(action)
			local dest = action.destination

			if not dest or #dest == 0 then
				error("Destination must be specified for move rules")
			end

			return true
		end,
	},
	script = {
		apply = function(action, ucore)
			return action.handler(ucore)
		end,
		validate = function(action)
			local scriptf = action.file
			local _, handler = assert(pcall(dofile, scriptf))

			-- The script is expected to return something that we
			-- can call that will take a ucore object.
			action.handler = handler
		end,
	},
}

-- These need to have matching methods on the ucore object.
local valid_matchfields = {
	comm = true,
	jail = true,
	path = true,
}

-- Our default action is to move all cores to /var/ucrash and add the pid to
-- uniquify each one in case we were to crash again.
local builtin = {
	match = {
		comm = ".*",
	},

	action = {
		type = "move",
		destination = "/var/ucrash/%n.%p",
	},
}

local Rule = {}
function Rule:new(name, matchers, action)
	if not next(matchers) then
		error("rule " .. name .. " has no specified match patterns")
	end
	if not next(action) then
		error("rule " .. name .. " has no specified action")
	end

	local handler = action_handlers[action.type]
	if not handler then
		error("rule " .. name .. " has an invalid action '" ..
		    action.type .. "'")
	elseif handler.validate then
		handler.validate(action)
	end

	local obj = setmetatable({}, self)
	self.__index = self

	obj.name = name
	obj.matchers = matchers
	obj.action = action
	return obj
end
function Rule:apply(ucore)
	local action = self.action
	local handler = action_handlers[action.type]

	return handler.apply(action, ucore)
end
function Rule:match(ucore)
	for field, reg in pairs(self.matchers) do
		local value = ucore[field](ucore)

		if not reg:find(value) then
			return false
		end
	end

	return true
end

local function process_rule(name, rule)
	local matchers = {}

	for type, pattern in pairs(rule.match) do
		if not valid_matchfields[type] then
			error("error processing rule " .. name ..
			    ", unknown field '" .. type .. "'")
		end

		matchers[type] = core.regcomp(pattern)
	end

	return Rule:new(name, matchers, rule.action)
end

function config.load()
	-- XXX: Actually load something when we get libucl
	return {
		process_rule("builtin", builtin),
	}
end

return config
