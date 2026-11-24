-- 阿里百炼 FunASR 实时流式识别（DashScope WebSocket）。
--
-- URL    wss://dashscope.aliyuncs.com/api-ws/v1/inference/
-- 鉴权   Authorization: Bearer <apiKey>
-- 流程   run-task -> task-started -> 二进制 PCM -> finish-task ->
--        result-generated* -> task-finished
-- 结果   payload.output.sentence { text, sentence_end, heartbeat }

local URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"
local SAMPLE_RATE = 16000
local STOP_TIMEOUT_MS = 8000
local MAX_PREBUFFER = 300
local DEFAULT_MODEL = "qwen-audio-3.1-asr-flash-streaming"
local CUSTOM_MODEL = "自定义"

local main = { tid = 0 }

local function log(level, message)
    pime.log(level, "asr-funasr", message)
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
local ready = false
local prebuffer = {}
local committed = ""
local preview = ""
local stop_pending = false
local finish_sent = false
local final_seen = false
local task_id = ""
local inbound = ""

local function pick_model(settings)
    local selected = setting(settings, "model")
    if selected == CUSTOM_MODEL then
        local custom = setting(settings, "custom_model")
        if custom ~= "" then return custom end
        return DEFAULT_MODEL
    end
    if selected == "" then return DEFAULT_MODEL end
    return selected
end

local function send_finish()
    if net_id == 0 or finish_sent then return end
    finish_sent = true
    pime.net.send_text(net_id, pime.json.encode({
        header = {action = "finish-task", task_id = task_id,
                  streaming = "duplex"},
        payload = {input = {}},
    }))
end

local function finish_session()
    if final_seen then return end
    final_seen = true
    local text = committed .. preview
    if text ~= "" then emit(session, text, true) end
    if net_id ~= 0 then
        pime.net.close(net_id)
        net_id = 0
    end
end

local function on_task_started()
    ready = true
    for _, pcm in ipairs(prebuffer) do pime.net.send_binary(net_id, pcm) end
    prebuffer = {}
    if stop_pending then
        stop_pending = false
        send_finish()
    end
end

local function on_result(output)
    local sentence = output.sentence
    if type(sentence) ~= "table" or sentence.heartbeat then return end
    local text = sentence.text
    if type(text) ~= "string" or text == "" then return end
    if sentence.sentence_end then
        committed = committed .. text
        preview = ""
        emit(session, committed, false)
    else
        preview = text
        emit(session, committed .. preview, false)
    end
end

local function handle_message(object)
    if type(object) ~= "table" then return end
    local header = object.header
    if type(header) ~= "table" then return end
    local event = header.event
    if event == "task-started" then
        on_task_started()
    elseif event == "result-generated" then
        local payload = object.payload
        if type(payload) == "table" and type(payload.output) == "table" then
            on_result(payload.output)
        end
    elseif event == "task-finished" then
        finish_session()
    elseif event == "task-failed" then
        log("error", "task failed [" .. tostring(header.error_code or "UNKNOWN")
            .. "]: " .. tostring(header.error_message or ""))
        finish_session()
    end
end

local function handle_text(data)
    inbound = inbound .. data
    local object = pime.json.decode(inbound)
    if type(object) == "table" then
        inbound = ""
        handle_message(object)
    elseif #inbound > 1048576 then
        log("warn", "discarding oversized server message")
        inbound = ""
    end
end

local function handle_event(event)
    if type(event) ~= "table" then return end
    if event.type == "net.data" then
        if type(event.data) == "string" then handle_text(event.data) end
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
    if api_key == "" then
        log("error", "未配置 API Key，请在插件设置中填写")
        return
    end

    local endpoint = setting(settings, "endpoint")
    if endpoint == "" then endpoint = URL end
    task_id = pime.random.bytes(16)
    if type(task_id) ~= "string" or task_id == "" then
        task_id = "pime-asr-" .. tostring(pime.time.wall_ms())
    end

    local id, error = pime.net.open("wss", endpoint, {
        headers = {Authorization = "Bearer " .. api_key},
        heartbeat_ms = 15000,
    })
    if not id then
        log("error", "connect failed: " .. tostring(error))
        return
    end
    net_id = id

    pime.net.send_text(net_id, pime.json.encode({
        header = {action = "run-task", task_id = task_id, streaming = "duplex"},
        payload = {
            task_group = "audio",
            task = "asr",
            ["function"] = "recognition",
            model = pick_model(settings),
            parameters = {format = "pcm", sample_rate = SAMPLE_RATE},
            input = {},
        },
    }))
    log("info", "listening")

    local stopping = false
    local deadline = 0
    while true do
        if not stopping then
            local frame = pime.audio.capture_read({timeout_ms = 60})
            if type(frame) == "table" and type(frame.data) == "string"
               and #frame.data > 0 then
                if ready then
                    pime.net.send_binary(net_id, frame.data)
                else
                    prebuffer[#prebuffer + 1] = frame.data
                    if #prebuffer > MAX_PREBUFFER then table.remove(prebuffer, 1) end
                end
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
                    if ready then send_finish() else stop_pending = true end
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

    if not final_seen then finish_session() end
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
            {name = "asr-funasr", entry = "asr_worker", priority = 1})
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
