--
-- Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
--
-- SPDX-License-Identifier: BSD-2-Clause
--

local config = {}
local core = require('core')
local ucl = require('ucl')

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

	local function pname(ucore)
		local comm = ucore:comm()

		return filename_part(comm)
	end

	-- If we want the replacement value to come from a function, then we
	-- want to avoid calling it up-front; just refer to the function here,
	-- and let the loop below lazily resolve the value if one of them was
	-- actually specified.
	--
	-- Any functions here should take a single argument, the ucore that we
	-- are examining.
	local symbols = {
		["%d"] = ucore.domainname,
		["%h"] = ucore.hostname,
		["%j"] = attrs.jid,
		["%n"] = pname,
		["%P"] = attrs.ppid,
		["%p"] = attrs.pid,
		["%s"] = attrs.signal,
	}

	for sym, val in pairs(symbols) do
		local symesc = "%" .. sym
		if path:find(symesc) then
			if type(val) == "function" then
				val = val(ucore)
			end

			path = path:gsub(symesc, tostring(val))
		end
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

	-- We only peeled apart the dir part to create it; path should still be
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

local function command_shell_split(command)
	local tbl = {}
	local arg = ""
	local escaped
	local quoted

	for i = 1, #command do
		local c = command:sub(i, i)

		if escaped then
			arg = arg .. c
			escaped = false
			goto next
		elseif c == "\\" then
			escaped = true
			goto next
		end

		if c == "\"" or c == "'" then
			if quoted then
				if c == quoted then
					quoted = nil
				else
					arg = arg .. c
				end
			else
				-- Quotes don't go into our argument, they just
				-- stop us from breaking whitespace mostly.
				quoted = c
			end

			goto next
		end

		-- Word split on spaces
		if c == " " and not quoted then
			tbl[#tbl + 1] = arg
			arg = ""
			goto next
		end

		-- Either we're quoted, or we're not a space.  Just append.
		arg = arg .. c

		::next::
	end

	if #arg > 0 then
		tbl[#tbl + 1] = arg
	end

	return tbl
end

local action_handlers = {
	discard = {
		apply = function(_, ucore)
			local path = ucore:path()
			local ok, err = os.remove(path)

			if not ok then
				core.error(path .. ": " .. err)
			else
				core.notice(path .. " discarded")
			end

			return ok
		end,
	},
	ignore = {
		apply = function(_, ucore)
			core.notice(ucore:path() .. " ignored")
			return true
		end,
	},
	move = {
		apply = function(action, ucore)
			local dest = process_destpath(ucore, action.destination)
			local path = (ucore:path() or "<shm>")
			local ok, err = ucore:move(dest)

			if not ok then
				core.error(path .. " move: " .. err)
			else
				core.notice(path .. " moved to " .. dest)
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
	pipe = {
		apply = function(action, ucore)
			local cmd = {}
			for _, arg in ipairs(action.command) do
				cmd[#cmd + 1] = replace_symbols(ucore, arg)
			end

			local path = ucore:path()
			local ok, err = ucore:pipe(table.unpack(cmd))
			local cmdname = cmd[1]

			if not ok then
				core.error(path .. " pipe (" .. cmdname ..
				    "): " .. err)
			else
				core.notice(path .. " piped to " ..
				    cmdname)
			end

			return ok
		end,
		validate = function(action)
			local command = action.command

			if type(command) ~= "table" then
				command = command_shell_split(command)
			end

			if #command == 0 then
				error("Pipe directives must specify a command")
			elseif not command[1]:match("^/") then
				error("Pipe commands must be an absolute path")
			end

			action.command = command
		end,
	},
	script = {
		apply = function(action, ucore)
			local ok = action.handler(ucore)
			core.notice(ucore:path() .. " handed over to " .. action.file)
			return ok
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
	domainname = true,
	hostname = true,
	jail = true,
	path = true,
}

local Rule = {}
function Rule:new(name, matchers, actions)
	if not next(matchers) then
		error("rule " .. name .. " has no specified match patterns")
	end
	if #actions == 0 then
		error("rule " .. name .. " has no specified action")
	end

	for _, action in ipairs(actions) do
		local handler = action_handlers[action.type]
		if not handler then
			error("rule " .. name .. " has an invalid action '" ..
				action.type .. "'")
		elseif handler.validate then
			handler.validate(action)
		end
	end

	local obj = setmetatable({}, self)
	self.__index = self

	obj.name = name
	obj.matchers = matchers
	obj.actions = actions
	return obj
end
function Rule:apply(ucore)
	for _, action in ipairs(self.actions) do
		local handler = action_handlers[action.type]

		if not handler.apply(action, ucore) then
			core.error(action.type .. " in current rule pipeline failed")
			return false
		end
	end

	return true
end
function Rule:match(ucore)
	for field, reg in pairs(self.matchers) do
		local value = ucore[field](ucore) or ""

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

	if #rule.execute == 0 and rule.execute['type'] ~= nil then
		rule.execute = { rule.execute }
	end

	return Rule:new(name, matchers, rule.execute)
end

function config.load()
	local parser = ucl.parser()
	local cfgfile = core.cfgfile

	local function cfg_error(msg)
		error(cfgfile .. ": " .. msg)
	end

	local ok, err = parser:parse_file(cfgfile)
	if not ok then
		cfg_error(err)
	end

	local cfg = parser:get_object()
	if not cfg['filters'] or type(cfg['filters']) ~= 'table' then
		cfg_errors("filters config must be an object or array")
	end

	-- A bit of syntactic sugar: a single filter may be specified as just an
	-- object assigned to the filters key.
	if #cfg['filters'] == 0 and cfg['filters']['match'] then
		cfg['filters'] = { cfg['filters'] }
	end
	if #cfg['filters'] == 0 then
		cfg_error("no filters provided")
	end

	local rules = {}
	for idx, filter in ipairs(cfg['filters']) do
		local name = cfg['name'] or ("rule #" .. tostring(idx))

		rules[#rules + 1] = process_rule(idx, filter)
	end

	return rules
end

return config
