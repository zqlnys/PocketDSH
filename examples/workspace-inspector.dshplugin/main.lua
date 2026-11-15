local config = {}

function activate(values)
  config = values or {}
  plugin.log("info", "Workspace Inspector activated")
end

function handle_tool(name, args)
  if name == "workspace_report" then
    local path = args.path or "."
    local depth = tonumber(args.depth) or 2
    local listing = plugin.workspace_list(path, depth)
    if listing == nil then
      return plugin.result("Workspace listing failed", true)
    end
    return plugin.result("Real workspace listing for " .. path .. ":\n" .. listing, false)
  end

  if name == "write_pocket_note" then
    local message = args.message or ""
    local path = args.path or "POCKET_NOTE.md"
    local prefix = config.prefix or "Pocket note"
    local body = "# " .. prefix .. "\n\n" .. message .. "\n"
    local result = plugin.workspace_write(path, body, true)
    if result == nil then
      return plugin.result("Could not write " .. path, true)
    end
    plugin.log("info", "Wrote " .. path)
    return plugin.result("Real file written: " .. path .. "\n" .. result, false)
  end

  return plugin.result("Unknown tool: " .. name, true)
end
