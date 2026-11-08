local key = "__relapsegtk_console_v1"
local env = getgenv()
local existing = env[key]
if existing then
    existing.publish()
    return
end

local safeCall = clonefunction(pcall)
local raise = clonefunction(error)
local stringify = clonefunction(tostring)
local running = clonefunction(coroutine.running)
local yield = clonefunction(coroutine.yield)
local resume = clonefunction(coroutine.resume)
local defer = clonefunction(task.defer)
local peers = {}
local waiting = {}
local pending = 0
local nextId = 0
local title = "Relapse Console"
local visible = false
local state = {}

local function bounded(text, limit)
    local truncated = #text > limit
    text = string.sub(text, 1, limit)
    if truncated then
        while #text > 0 and not utf8.len(text) and #text > limit - 4 do
            text = string.sub(text, 1, -2)
        end
    end
    if not utf8.len(text) then
        text = string.gsub(text, "[\128-\255]", function(c)
            return string.format("\\x%02x", string.byte(c))
        end)
    end
    text = string.gsub(text, "[%z\1-\8\11-\31\127]", function(c)
        return string.format("\\x%02x", string.byte(c))
    end)
    if #text > limit - 20 then
        text = string.sub(text, 1, limit - 20)
        while not utf8.len(text) do text = string.sub(text, 1, -2) end
        truncated = true
    end
    return text .. (truncated and " … [truncated]" or "")
end

local function format(...)
    local parts, bytes = {}, 0
    local count = select("#", ...)
    for i = 1, math.min(count, 64) do
        local ok, text = safeCall(stringify, select(i, ...))
        text = bounded(ok and text or "<tostring failed>", 4096)
        parts[#parts + 1] = text
        bytes += #text + 1
        if bytes >= 4096 then break end
    end
    if count > #parts then parts[#parts + 1] = "…" end
    return bounded(table.concat(parts, "\t"), 4096)
end

local function send(connection, message)
    return safeCall(connection.Send, connection, message)
end

local function detach(connection)
    peers[connection] = nil
end

local function hasPeers()
    local active = false
    for connection in peers do
        if not connection.Connected then
            detach(connection)
        else
            active = true
        end
    end
    return active
end

local function broadcast(message)
    local active = false
    for connection in peers do
        if not connection.Connected then
            detach(connection)
        elseif send(connection, message) then
            active = true
        end
    end
    return active
end

local function resumeWith(thread, text)
    local ok, failure = resume(thread, text)
    if not ok then raise(failure, 0) end
end

local function finish(id, text)
    local thread = waiting[id]
    if not thread then return end
    waiting[id] = nil
    pending -= 1
    defer(resumeWith, thread, text)
end

local function finishAll()
    for id in waiting do finish(id, "") end
end

local function emit(level, ...)
    broadcast({ type = "print", level = level, text = format(...) })
end

function state.show()
    visible = true
    broadcast({ type = "show" })
end

function state.hide()
    finishAll()
    visible = false
    broadcast({ type = "hide" })
end

function state.clear()
    broadcast({ type = "clear" })
end

function state.name(value)
    if type(value) ~= "string" then raise("rconsolename: string expected", 2) end
    title = bounded(value, 255)
    broadcast({ type = "title", text = title })
end

function state.input()
    if pending >= 64 then return "" end
    local thread = running()
    if not thread or not hasPeers() then return "" end
    repeat nextId = nextId % 2147483647 + 1 until not waiting[nextId]
    local id = nextId
    waiting[id] = thread
    pending += 1
    visible = true
    broadcast({ type = "show" })
    if not broadcast({ type = "input", id = id }) then
        waiting[id] = nil
        pending -= 1
        return ""
    end
    return yield()
end

function state.publish()
    if state.channel and state.channel.IsPublished then return end
    local channel = RelapseChannel.new("relapsegtk.console.v1")
    state.channel = channel
    channel.Connected:Connect(function(connection)
        peers[connection] = true
        connection.Disconnected:Connect(function()
            detach(connection)
            if not hasPeers() then finishAll() end
        end)
        connection.MessageReceived:Connect(function(message)
            if type(message) ~= "table" or message.op ~= "input_response" or
                type(message.id) ~= "number" or message.id % 1 ~= 0 or
                message.id < 1 or message.id > 2147483647 or
                type(message.text) ~= "string" or #message.text > 4096 then
                connection:Close()
                return
            end
            finish(message.id, message.text)
        end)
        if not send(connection, { type = "state", title = title, visible = visible }) then
            detach(connection)
            connection:Close()
            return
        end
        for id in waiting do send(connection, { type = "input", id = id }) end
    end)
end

env.rconsoleprint = function(...) emit("print", ...) end
env.rconsoleinfo = function(...) emit("info", ...) end
env.rconsolewarn = function(...) emit("warn", ...) end
env.rconsoleerr = function(...) emit("error", ...) end
env.rconsoleinput = state.input
env.rconsoleclear = state.clear
env.rconsolename = state.name
env.rconsolehide = state.hide
env.rconsoleshow = state.show
env[key] = state
state.publish()
