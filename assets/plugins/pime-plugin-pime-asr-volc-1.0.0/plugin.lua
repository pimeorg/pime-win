-- 火山引擎 bigmodel_async 在线流式语音识别。
--
-- 事件（核心 -> 插件，owner 线程）：voice { action: 0 start | 1 stop | 2 cancel }
-- 音频：插件在 worker 线程用 pime.audio.capture_read 拉取 16 kHz 单声道 s16le。
-- 网络：pime.net.open("wss", ...) + 二进制帧；服务端事件通过 pime.on_event
--       回到 owner 线程，再转交给 worker。结果用 pime.emit 异步上屏。
--
-- 协议（官方 sauc_websocket_demo.py）：4 字节头 + int32 序号 + uint32 长度 +
-- gzip(JSON/PCM)。消息类型 0x1 配置 / 0x2 音频 / 0x9 结果 / 0xF 错误；
-- 最后一包序号为负、flags=0x3。

local URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
local SAMPLE_RATE = 16000
local STOP_TIMEOUT_MS = 8000

local main = { tid = 0 }

local function log(level, message)
    pime.log(level, "asr-volc", message)
end

local function uuid()
    local bytes = pime.random.bytes(16)
    if type(bytes) ~= "string" or #bytes < 16 then
        return "00000000-0000-4000-8000-000000000000"
    end
    local hex = {}
    for i = 1, 16 do hex[i] = string.format("%02x", bytes:byte(i)) end
    local s = table.concat(hex)
    return table.concat({s:sub(1, 8), s:sub(9, 12), s:sub(13, 16),
                         s:sub(17, 20), s:sub(21, 32)}, "-")
end

local function emit(session, text, commit)
    if not session or session == 0 then return end
    local commands = {}
    if type(text) == "string" and text ~= "" then
        if commit then
            commands[1] = {type = "commit", text = text}
        else
            commands[1] = {type = "set_composition", text = text, cursor = #text}
        end
    end
    pime.emit(session, {handled = true, commands = commands})
end

local function setting(settings, key)
    local value = settings[key]
    if type(value) == "string" then return value end
    return ""
end

-- ---------------------------------------------------------------- worker

local net_id = 0
local session = 0
local seq = 1
local partial = ""
local finishing = false
local final_seen = false

local function read_u32(text, offset)
    local a, b, c, d = text:byte(offset, offset + 3)
    if not a then return nil end
    return a * 16777216 + b * 65536 + c * 256 + d
end

local function read_i32(text, offset)
    local value = read_u32(text, offset)
    if not value then return nil end
    if value >= 2147483648 then return value - 4294967296 end
    return value
end

local function send_frame(message_type, value, ser, comp, payload)
    if net_id == 0 then return false end
    local flags = value < 0 and 3 or 1
    local frame = string.char(0x11, message_type * 16 + flags,
                              ser * 16 + comp, 0)
        .. string.pack(">i4", value)
        .. string.pack(">I4", #payload)
        .. payload
    return pime.net.send_binary(net_id, frame)
end

local function send_audio(pcm)
    local gz = pime.compress.gzip(pcm)
    if not gz then
        log("error", "gzip failed")
        return
    end
    send_frame(2, seq, 0, 1, gz)
    seq = seq + 1
end

local function finish_session()
    if final_seen then return end
    final_seen = true
    local text = partial
    if text ~= "" then emit(session, text, true) end
    if net_id ~= 0 then
        pime.net.close(net_id)
        net_id = 0
    end
end

local function on_result(text, last)
    if type(text) ~= "string" then text = "" end
    if text ~= "" then partial = text end
    if last then
        finish_session()
    elseif partial ~= "" then
        emit(session, partial, false)
    end
end

local function parse_server_frame(data)
    if #data < 8 then return end
    local b0, b1, b2 = data:byte(1, 3)
    local header = (b0 % 16) * 4
    local message_type = math.floor(b1 / 16) % 16
    local flags = b1 % 16
    local ser = math.floor(b2 / 16) % 16
    local comp = b2 % 16
    local offset = header + 1
    if flags % 2 == 1 then offset = offset + 4 end
    local last = math.floor(flags / 2) % 2 == 1
    if math.floor(flags / 4) % 2 == 1 then offset = offset + 4 end

    if message_type == 0x9 then
        local size = read_u32(data, offset)
        if not size then return end
        offset = offset + 4
        local payload = data:sub(offset, offset + size - 1)
        if comp == 1 then
            local plain = pime.compress.gunzip(payload)
            if not plain then
                log("warn", "gunzip failed")
                return
            end
            payload = plain
        end
        if ser == 1 then
            local object = pime.json.decode(payload)
            if type(object) == "table" then
                local result = object.result
                local text = type(result) == "table" and result.text or nil
                on_result(text, last)
            elseif last then
                finish_session()
            end
        end
    elseif message_type == 0xF then
        local code = read_i32(data, offset)
        local size = read_u32(data, offset + 4) or 0
        local message = data:sub(offset + 8, offset + 8 + size - 1)
        log("error", "server error " .. tostring(code) .. ": " .. tostring(message))
        finish_session()
    end
end

local function handle_event(event)
    if type(event) ~= "table" then return end
    if event.type == "net.data" then
        if type(event.data) == "string" then parse_server_frame(event.data) end
    elseif event.type == "net.close" then
        net_id = 0
        if (event.error or 0) ~= 0 then
            log("warn", "connection closed: " .. tostring(event.error))
        end
    end
end

function asr_worker(tid)
    local first = pime.thread.recv(tid, 5000)
    if type(first) ~= "table" or first.cmd ~= "start" then return end
    session = first.session or 0
    local settings = first.settings or {}
    local api_key = setting(settings, "api_key")
    local app_key = setting(settings, "app_key")
    local access_key = setting(settings, "access_key")
    if api_key == "" and (app_key == "" or access_key == "") then
        log("error", "未配置 API Key，请在插件设置中填写")
        return
    end

    local resource = setting(settings, "resource_id")
    if resource == "" then resource = "volc.seedasr.sauc.duration" end
    local endpoint = setting(settings, "endpoint")
    if endpoint == "" then endpoint = URL end

    local headers = {
        ["X-Api-Resource-Id"] = resource,
        ["X-Api-Request-Id"] = uuid(),
        ["X-Api-Connect-Id"] = uuid(),
        ["X-Api-Sequence"] = "-1",
    }
    if api_key ~= "" then
        headers["X-Api-Key"] = api_key
    else
        headers["X-Api-App-Key"] = app_key
        headers["X-Api-Access-Key"] = access_key
    end

    local id, error = pime.net.open("wss", endpoint,
                                    {headers = headers, heartbeat_ms = 15000})
    if not id then
        log("error", "connect failed: " .. tostring(error))
        return
    end
    net_id = id

    local config = pime.json.encode({
        user = {uid = app_key ~= "" and app_key or "pime"},
        audio = {format = "pcm", codec = "raw", rate = SAMPLE_RATE,
                 bits = 16, channel = 1},
        request = {model_name = "bigmodel", enable_itn = true,
                   enable_punc = true, enable_ddc = false,
                   show_utterances = false, enable_nonstream = false},
    })
    local init = pime.compress.gzip(config)
    if not init then
        log("error", "gzip failed")
        pime.net.close(net_id)
        net_id = 0
        return
    end
    seq = 1
    send_frame(1, seq, 1, 1, init)
    seq = seq + 1
    log("info", "listening")

    local stopping = false
    local deadline = 0
    while true do
        if not stopping then
            local frame = pime.audio.capture_read({timeout_ms = 60})
            if type(frame) == "table" and type(frame.data) == "string"
               and #frame.data > 0 then
                send_audio(frame.data)
            end
        end

        local wait = stopping and 100 or 0
        for _ = 1, 8 do
            local message = pime.thread.recv(tid, wait)
            if type(message) ~= "table" then break end
            wait = 0
            if message.cmd == "stop" then
                if not stopping then
                    stopping = true
                    local last = pime.compress.gzip("")
                    if last then
                        send_frame(2, -seq, 0, 1, last)
                    end
                end
            elseif message.cmd == "cancel" then
                if net_id ~= 0 then
                    pime.net.close(net_id)
                    net_id = 0
                end
                final_seen = true
                return
            elseif message.cmd == "net" then
                handle_event(message.event)
            end
            if final_seen or net_id == 0 then break end
        end

        if final_seen or net_id == 0 then break end
        if stopping then
            if deadline == 0 then
                deadline = pime.time.monotonic_ms() + STOP_TIMEOUT_MS
            elseif pime.time.monotonic_ms() >= deadline then
                log("warn", "no final result before timeout")
                break
            end
        end
    end

    if not final_seen then
        finishing = true
        finish_session()
    end
    if net_id ~= 0 then
        pime.net.close(net_id)
        net_id = 0
    end
end

-- ----------------------------------------------------------------- owner

local function stop_worker()
    if main.tid == 0 then return end
    -- A previous turn that is still closing must not commit its partial text.
    pime.thread.post(main.tid, {cmd = "cancel"}, 200)
    pime.thread.close(main.tid)
    pime.thread.join(main.tid, 1500)
    main.tid = 0
end

function pime.process(input)
    local event = input.event
    if type(event) ~= "table" or event.type ~= "voice" then
        return {handled = false, commands = {}}
    end
    local action = event.action or 0
    if action == 0 then
        stop_worker()
        local started, error = pime.audio.capture_start(
            {rate = SAMPLE_RATE, channels = 1, format = 0})
        if not started then
            log("error", "microphone unavailable: " .. tostring(error))
            return {handled = true, commands = {}}
        end
        -- input.config is the generation snapshot with the settings flat.
        local settings = input.config
        if type(settings) ~= "table" then settings = {} end
        local tid, thread_error = pime.thread.create(
            {name = "asr-volc", entry = "asr_worker", priority = 1})
        if not tid then
            pime.audio.capture_stop()
            log("error", "worker failed: " .. tostring(thread_error))
            return {handled = true, commands = {}}
        end
        main.tid = tid
        pime.thread.post(tid, {cmd = "start", session = input.session or 0,
                               settings = settings}, 500)
        return {handled = true, commands = {}}
    end

    pime.audio.capture_stop()
    if main.tid ~= 0 then
        pime.thread.post(main.tid, {cmd = action == 1 and "stop" or "cancel"}, 500)
    end
    return {handled = true, commands = {}}
end

function pime.on_event(input)
    local event = input.event
    if type(event) ~= "table" then return {handled = false, commands = {}} end
    local kind = event.type
    if kind ~= "net.data" and kind ~= "net.close" and kind ~= "net.open" then
        return {handled = false, commands = {}}
    end
    if main.tid ~= 0 then
        pime.thread.post(main.tid, {cmd = "net", event = event}, 500)
    end
    return {handled = false, commands = {}}
end

function pime.shutdown()
    pime.audio.capture_stop()
    if main.tid ~= 0 then
        pime.thread.post(main.tid, {cmd = "cancel"}, 200)
        pime.thread.close(main.tid)
        pime.thread.join(main.tid, 1500)
        main.tid = 0
    end
end
