--[[[
-- Just a test for TCP API
--]]

local rspamd_tcp = require "rspamd_tcp"
local logger = require "rspamd_logger"
local tcp_sync = require "lua_tcp_sync"

-- [[ old fashioned callback api ]]
local function http_simple_tcp_async_symbol(task)
  logger.errx(task, 'http_tcp_symbol: begin')
  local function http_get_cb(err, data, conn)
    logger.errx(task, 'http_get_cb: got reply: %s, error: %s, conn: %s', data, err, conn)
    task:insert_result('HTTP_ASYNC_RESPONSE_2', 1.0, data)
  end
  local function http_read_post_cb(err, conn)
    logger.errx(task, 'http_read_post_cb: write done: error: %s, conn: %s', err, conn)
    conn:add_read(http_get_cb)
  end
  local function http_read_cb(err, data, conn)
    logger.errx(task, 'http_read_cb: got reply: %s, error: %s, conn: %s', data, err, conn)
    conn:add_write(http_read_post_cb, "POST /request2 HTTP/1.1\r\n\r\n")
    task:insert_result('HTTP_ASYNC_RESPONSE', 1.0, data or err)
  end
  rspamd_tcp:request({
    task = task,
    callback = http_read_cb,
    host = '127.0.0.1',
    data = {'GET /request HTTP/1.1\r\nConnection: keep-alive\r\n\r\n'},
    read = true,
    port = 18080,
  })
end

local function http_simple_tcp_symbol(task)
  logger.errx(task, 'connect_sync, before')

  local err
  local is_ok, connection = tcp_sync.connect {
    task = task,
    host = '127.0.0.1',
    timeout = 20,
    port = 18080,
  }

  if not is_ok then
    task:insert_result('HTTP_SYNC_WRITE_ERROR', 1.0, connection)
    logger.errx(task, 'write error: %1', connection)
  end

  logger.errx(task, 'connect_sync %1, %2', is_ok, tostring(connection))

  is_ok, err = connection:write('GET /request_sync HTTP/1.1\r\nConnection: keep-alive\r\n\r\n')

  logger.errx(task, 'write %1, %2', is_ok, err)
  if not is_ok then
    task:insert_result('HTTP_SYNC_WRITE_ERROR', 1.0, err)
    logger.errx(task, 'write error: %1', err)
  end

  local data
  local got_content = ''
  repeat
    is_ok, data = connection:read_once();
    logger.errx(task, 'read_once: is_ok: %1, data: %2', is_ok, data)
    if not is_ok then
      task:insert_result('HTTP_SYNC_ERROR', 1.0, data)
      return
    else
      got_content = got_content .. data
    end
    if got_content:find('hello') then
      -- dummy_http.py responds with either hello world or hello post
      break
    end
  until false

  task:insert_result('HTTP_SYNC_RESPONSE', 1.0, got_content)

  is_ok, err = connection:write("POST /request2 HTTP/1.1\r\n\r\n")
  logger.errx(task, 'write[2] %1, %2', is_ok, err)

  got_content = ''
  repeat
    is_ok, data = connection:read_once();
    logger.errx(task, 'read_once[2]: is_ok %1, data: %2', is_ok, data)
    if not is_ok then
      task:insert_result('HTTP_SYNC_ERROR_2', 1.0, data)
      return
    else
      got_content = got_content .. data
    end
    if got_content:find('hello') then
      -- dummy_http.py responds with either hello world or hello post
      break
    end
  until false

  task:insert_result('HTTP_SYNC_RESPONSE_2', 1.0, data)

  connection:close()
end

local function http_tcp_symbol(task)
  local url = tostring(task:get_request_header('url'))
  local method = tostring(task:get_request_header('method'))

  if url == 'nil' then
    return
  end

  local err
  local is_ok, connection = tcp_sync.connect {
    task = task,
    host = '127.0.0.1',
    timeout = 20,
    port = 18080,
  }

  logger.errx(task, 'connect_sync %1, %2', is_ok, tostring(connection))
  if not is_ok then
    logger.errx(task, 'connect error: %1', connection)
    return
  end

  is_ok, err = connection:write(string.format('%s %s HTTP/1.1\r\nConnection: close\r\n\r\n', method:upper(), url))

  logger.errx(task, 'write %1, %2', is_ok, err)
  if not is_ok then
    logger.errx(task, 'write error: %1', err)
    return
  end

  local content_length, content

  while true do
    local header_line
    is_ok, header_line = connection:read_until("\r\n")
    if not is_ok then
      logger.errx(task, 'failed to get header: %1', header_line)
      return
    end

    if header_line == "" then
      logger.errx(task, 'headers done')
      break
    end

    local value
    local header = header_line:gsub("([%w-]+): (.*)",
        function (h, v) value = v; return h:lower() end)

    logger.errx(task, 'parsed header: %1 -> "%2"', header, value)

    if header == "content-length" then
      content_length = tonumber(value)
    end

  end

  if content_length then
    is_ok, content = connection:read_bytes(content_length)
    if is_ok then
      task:insert_result('HTTP_SYNC_CONTENT_' .. method, 1.0, content)
    end
  else
    is_ok, content = connection:read_until_eof()
    if is_ok then
      task:insert_result('HTTP_SYNC_EOF_' .. method, 1.0, content)
    end
  end
  logger.errx(task, '(is_ok: %1) content [%2 bytes] %3', is_ok, content_length, content)
end

rspamd_config:register_symbol({
  name = 'SIMPLE_TCP_ASYNC_TEST',
  score = 1.0,
  callback = http_simple_tcp_async_symbol,
  no_squeeze = true
})
rspamd_config:register_symbol({
  name = 'SIMPLE_TCP_TEST',
  score = 1.0,
  callback = http_simple_tcp_symbol,
  no_squeeze = true,
  flags = 'coro',
})

rspamd_config:register_symbol({
  name = 'HTTP_TCP_TEST',
  score = 1.0,
  callback = http_tcp_symbol,
  no_squeeze = true,
  flags = 'coro',
})
-- ]]
