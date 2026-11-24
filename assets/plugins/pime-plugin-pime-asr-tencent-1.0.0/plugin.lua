-- 腾讯云实时语音识别 V2（WebSocket）。
--
-- URL    wss://asr.cloud.tencent.com/asr/v2/<appid>?<参数>&signature=...
-- 签名   除 signature 外参数按键字典序拼接（值为原文）：
--        "asr.cloud.tencent.com/asr/v2/<appid>?k1=v1&..."，
--        HMAC-SHA1(SecretKey, 原文) -> Base64 -> URL 编码。
-- 握手   服务端先回 {"code":0} 文本帧，之后才允许发 PCM 二进制帧。
-- 结束   发送 {"type":"end"}，服务端回 final=1 后断开。
-- 结果   sentences[].sentence_type 0=不确定 1=确定；final=1 为末包。

local HOST_PATH = "asr.cloud.tencent.com/asr/v2/"
local SAMPLE_RATE = 16000
local STOP_TIMEOUT_MS = 8000
local MAX_PREBUFFER = 300

local main = { tid = 0 }

local function log(level, message)
    pime.log(level, "asr-tencent", message)
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

local function percent_encode(text)
    local out = {}
    for i = 1, #text do
        local b = text:byte(i)
        if (b >= 48 and b <= 57) or (b >= 65 and b <= 90)
           or (b >= 97 and b <= 122) or b == 45 or b == 46 or b == 95
           or b == 126 then
            out[#out + 1] = string.char(b)
        else
            out[#out + 1] = string.format("%%%02X", b)
        end
    end
    return table.concat(out)
end

local function build_query(params, encode_values)
    local keys = {}
    for key in pairs(params) do keys[#keys + 1] = key end
    table.sort(keys)
    local parts = {}
    for _, key in ipairs(keys) do
        local value = tostring(params[key])
        if encode_values then value = percent_encode(value) end
        parts[#parts + 1] = key .. "=" .. value
    end
    return table.concat(parts, "&")
end

local ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64(data)
    local out = {}
    local i = 1
    while i <= #data do
        local a, b, c = data:byte(i, i + 2)
        local value = (a << 16) | ((b or 0) << 8) | (c or 0)
        local c1 = ((value >> 18) & 63) + 1
        local c2 = ((value >> 12) & 63) + 1
        local c3 = ((value >> 6) & 63) + 1
        local c4 = (value & 63) + 1
        out[#out + 1] = ALPHABET:sub(c1, c1)
        out[#out + 1] = ALPHABET:sub(c2, c2)
        out[#out + 1] = b and ALPHABET:sub(c3, c3) or "="
        out[#out + 1] = c and ALPHABET:sub(c4, c4) or "="
        i = i + 3
    end
    return table.concat(out)
end

-- ---------------------------------------------------------------- worker

local net_id = 0
local session = 0
local ready = false
local prebuffer = {}
local committed = ""
local preview = ""
local finishing = false
local stop_pending = false
local end_sent = false
local final_seen = false
local inbound = ""

local function send_end()
    if net_id == 0 or end_sent then return end
    end_sent = true
    pime.net.send_text(net_id, pime.json.encode({type = "end"}))
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

local function handle_sentence(sentence)
    if type(sentence) ~= "table" then return end
    local text = sentence.sentence
    if type(text) ~= "string" or text == "" then return end
    local is_final = (sentence.sentence_type or 0) == 1
    if is_final then
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
    local code = object.code or 0
    if code ~= 0 then
        log("error", "server error " .. tostring(code) .. ": "
            .. tostring(object.message or ""))
        finish_session()
        return
    end
    if not ready then
        ready = true
        for _, pcm in ipairs(prebuffer) do pime.net.send_binary(net_id, pcm) end
        prebuffer = {}
        if stop_pending then
            stop_pending = false
            send_end()
        end
    end
    local sentences = object.sentences
    if type(sentences) == "table" then
        if sentences.sentence ~= nil then
            handle_sentence(sentences)
        else
            for _, sentence in ipairs(sentences) do handle_sentence(sentence) end
        end
    end
    if object.final == 1 then finish_session() end
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
    local app_id = setting(settings, "app_id")
    local secret_id = setting(settings, "secret_id")
    local secret_key = setting(settings, "secret_key")
    if app_id == "" or secret_id == "" or secret_key == "" then
        log("error", "未配置 AppID / SecretId / SecretKey，请在插件设置中填写")
        return
    end

    local engine = setting(settings, "engine_model_type")
    if engine == "" then engine = "16k_zh_en_2.0" end

    local voice_id = pime.random.bytes(16)
    if type(voice_id) ~= "string" then voice_id = "voice" end

    local timestamp = pime.time.wall_ms() // 1000
    local nonce = 1
    local noise = pime.random.bytes(4)
    if type(noise) == "string" and #noise == 4 then
        local a, b, c, d = noise:byte(1, 4)
        nonce = 1 + (a * 16777216 + b * 65536 + c * 256 + d) % 1000000000
    end

    local params = {
        secretid = secret_id,
        timestamp = timestamp,
        expired = timestamp + 86400,
        nonce = nonce,
        engine_model_type = engine,
        voice_id = voice_id,
        voice_format = 1,
        needvad = 1,
    }
    local hotwords = setting(settings, "hotword_list")
    if hotwords ~= "" then params.hotword_list = hotwords end

    local sign_text = HOST_PATH .. app_id .. "?" .. build_query(params, false)
    local mac, mac_error = pime.crypto.mac("hmac-sha1", secret_key, sign_text)
    if not mac then
        log("error", "signature failed: " .. tostring(mac_error))
        return
    end
    local signature = percent_encode(base64(mac))
    local url = "wss://" .. HOST_PATH .. app_id .. "?"
        .. build_query(params, true) .. "&signature=" .. signature

    local id, error = pime.net.open("wss", url, {heartbeat_ms = 15000})
    if not id then
        log("error", "connect failed: " .. tostring(error))
        return
    end
    net_id = id
    log("info", "connecting")

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
                    if ready then send_end() else stop_pending = true end
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
            {name = "asr-tencent", entry = "asr_worker", priority = 1})
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
