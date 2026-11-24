local key = "__relapsegtk_dialogs_v1"
local env = getgenv()
local existing = env[key]
if existing then
    existing.publish()
    return
end

local safeCall = clonefunction(pcall)
local raise = clonefunction(error)
local running = clonefunction(coroutine.running)
local yield = clonefunction(coroutine.yield)
local resume = clonefunction(coroutine.resume)
local status = clonefunction(coroutine.status)
local defer = clonefunction(task.defer)
local spawn = clonefunction(task.spawn)
local delay = clonefunction(task.delay)
local cancel = clonefunction(task.cancel)
local peers = {}
local waiting = {}
local pending = 0
local nextId = 0
local objects = setmetatable({}, { __mode = "k" })
local state = {}

local icons = { Error = "error", Question = "question", Warning = "warning", Information = "information" }
local validIcons = { none = true, error = true, question = true, warning = true, information = true }
local buttonSets = {
    { "OK" }, { "OK", "Cancel" }, { "Abort", "Retry", "Ignore" },
    { "Yes", "No", "Cancel" }, { "Yes", "No" }, { "Retry", "Cancel" },
    { "Cancel", "Try Again", "Continue" },
}
local buttonIds = { OK = 1, Cancel = 2, Abort = 3, Retry = 4, Ignore = 5, Yes = 6, No = 7, ["Try Again"] = 10, Continue = 11 }

local function text(value, name, limit)
    if type(value) ~= "string" then raise(name .. ": string expected", 3) end
    if #value > limit or string.find(value, "%z") or not utf8.len(value) then
        raise(name .. ": expected UTF-8 text of at most " .. limit .. " bytes without NUL", 3)
    end
    return value
end

local function send(connection, message)
    return safeCall(connection.Send, connection, message)
end

local function complete(request, choice)
    if waiting[request.id] ~= request then return end
    waiting[request.id] = nil
    pending -= 1
    if request.timer then safeCall(cancel, request.timer) end
    if request.config then request.config.active = nil end
    if not send(request.connection, { type = "close", id = request.id }) and request.connection.Connected then
        safeCall(request.connection.Close, request.connection)
    end
    local result = request.result(choice)
    local callbacks = request.config and table.clone(request.config.callbacks) or {}
    defer(function()
        for _, callback in callbacks do
            spawn(function()
                local ok, failure = safeCall(callback, result)
                if not ok then defer(raise, failure, 0) end
            end)
        end
        if request.thread and status(request.thread) == "suspended" then
            local ok, failure = resume(request.thread, result)
            if not ok then raise(failure, 0) end
        end
    end)
end

local function open(config, result, thread)
    if pending >= 64 then raise("messagebox: too many pending dialogs", 3) end
    local connection
    for peer in peers do
        if peer.Connected then connection = peer; break end
        peers[peer] = nil
    end
    if not connection then raise("messagebox: RelapseGTK is not connected", 3) end
    repeat nextId = nextId % 2147483647 + 1 until not waiting[nextId]
    local request = { id = nextId, connection = connection, result = result, thread = thread }
    waiting[request.id] = request
    pending += 1
    if not send(connection, {
        type = "open", id = request.id, title = config.title, text = config.text,
        buttons = config.buttons, icon = config.icon, default = config.default,
    }) then
        waiting[request.id] = nil
        pending -= 1
        raise("messagebox: could not send the dialog request", 3)
    end
    request.count = #config.buttons
    return request
end

local function messagebox(body, title, flags)
    body = text(body, "messagebox text", 4096)
    title = text(title, "messagebox title", 255)
    flags = flags == nil and 0 or flags
    if type(flags) ~= "number" or flags % 1 ~= 0 or flags < 0 or flags > 4294967295 then
        raise("messagebox: flags must be an unsigned 32-bit integer", 2)
    end
    local buttons = buttonSets[flags % 16 + 1]
    if not buttons then raise("messagebox: unsupported button flags", 2) end
    local icon = ({ [16] = "error", [32] = "question", [48] = "warning", [64] = "information" })[flags % 256 - flags % 16] or "none"
    local default = math.floor(flags / 256) % 4 + 1
    local closed = table.find(buttons, "Cancel") or table.find(buttons, "No") or 1
    local thread = running()
    if not thread then raise("messagebox: a yieldable coroutine is required", 2) end
    open({ title = title, text = body, buttons = buttons, icon = icon, default = default <= #buttons and default or 1 },
        function(choice) return buttonIds[buttons[type(choice) == "number" and choice > 0 and choice or closed]] end, thread)
    return yield()
end

local methods = {}

function methods:Show()
    local config = objects[self]
    if not config then raise("MessageBox expected", 2) end
    if config.active then raise("MessageBox is already showing", 2) end
    local thread = config.yield and running() or nil
    if config.yield and not thread then raise("MessageBox:Show requires a yieldable coroutine", 2) end
    local request = open(config, function(choice)
        if type(choice) == "string" then return choice end
        return choice == 0 and "Closed" or config.buttons[choice]
    end, thread)
    request.config = config
    config.active = request
    if config.duration then
        request.timer = delay(config.duration, function()
            request.timer = nil
            complete(request, "TimedOut")
        end)
    end
    if config.yield then return yield() end
    return nil
end

function methods:Close()
    local config = objects[self]
    if not config then raise("MessageBox expected", 2) end
    if config.active then complete(config.active, "ForceClosed") end
end

function methods:OnChoice(callback)
    local config = objects[self]
    if not config then raise("MessageBox expected", 2) end
    if type(callback) ~= "function" then raise("MessageBox:OnChoice expects a function", 2) end
    table.insert(config.callbacks, callback)
    return self
end

local function new(options)
    if type(options) ~= "table" then raise("MessageBox.new expects an options table", 2) end
    local config = {
        title = text(options.Title == nil and "Relapse" or options.Title, "MessageBox Title", 255),
        text = text(options.Text == nil and "" or options.Text, "MessageBox Text", 4096),
        buttons = {}, icon = options.Icon == nil and "none" or options.Icon, default = 1,
        yield = options.Yield == nil or options.Yield, duration = options.Duration, callbacks = {},
    }
    local buttons = options.Buttons == nil and { "OK" } or options.Buttons
    if type(buttons) ~= "table" or #buttons < 1 or #buttons > 16 then
        raise("MessageBox Buttons must contain 1-16 labels", 2)
    end
    for i = 1, #buttons do
        config.buttons[i] = text(buttons[i], "MessageBox button", 128)
        if config.buttons[i] == "" then raise("MessageBox button labels cannot be empty", 2) end
    end
    if type(config.icon) ~= "string" or not validIcons[config.icon] then raise("MessageBox Icon is invalid", 2) end
    if type(config.yield) ~= "boolean" then raise("MessageBox Yield must be a boolean", 2) end
    if config.duration ~= nil and (type(config.duration) ~= "number" or config.duration ~= config.duration or
        config.duration < 0 or config.duration == math.huge) then
        raise("MessageBox Duration must be a finite nonnegative number", 2)
    end
    if options.Callback ~= nil then
        if type(options.Callback) ~= "function" then raise("MessageBox Callback must be a function", 2) end
        config.callbacks[1] = options.Callback
    end
    local object = setmetatable({}, { __index = methods })
    objects[object] = config
    return object
end

function state.publish()
    if state.channel and state.channel.IsPublished then return end
    local channel = RelapseChannel.new("relapsegtk.dialogs.v1")
    state.channel = channel
    channel.Connected:Connect(function(connection)
        peers[connection] = true
        connection.Disconnected:Connect(function()
            peers[connection] = nil
            for _, request in waiting do
                if request.connection == connection then complete(request, "ForceClosed") end
            end
        end)
        connection.MessageReceived:Connect(function(message)
            if type(message) ~= "table" or message.op ~= "choice" or type(message.id) ~= "number" or
                message.id % 1 ~= 0 or message.id < 1 or message.id > 2147483647 or
                type(message.choice) ~= "number" or message.choice % 1 ~= 0 or message.choice < 0 or message.choice > 16 then
                connection:Close()
                return
            end
            local request = waiting[message.id]
            if request and request.connection == connection then
                if message.choice > request.count then connection:Close(); return end
                complete(request, message.choice)
            end
        end)
    end)
end

env.messagebox = messagebox
env.MessageBox = { new = new, Icons = table.freeze(icons) }
env[key] = state
state.publish()
