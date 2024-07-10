--[[
Copyright (c) 2022, Vsevolod Stakhov <vsevolod@rspamd.com>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
]]--

if confighelp then
  return
end

-- MX check plugin
local rspamd_logger = require "rspamd_logger"
local rspamd_tcp = require "rspamd_tcp"
local rspamd_util = require "rspamd_util"
local lua_util = require "lua_util"
local lua_redis = require "lua_redis"
local N = "mx_check"
local fun = require "fun"

local settings = {
  timeout = 1.0, -- connect timeout
  symbol_bad_mx = 'MX_INVALID',
  symbol_no_mx = 'MX_MISSING',
  symbol_good_mx = 'MX_GOOD',
  symbol_white_mx = 'MX_WHITE',
  expire = 86400, -- 1 day by default
  expire_novalid = 7200, -- 2 hours by default for no valid mxes
  greylist_invalid = true, -- Greylist first message with invalid MX (require greylist plugin)
  key_prefix = 'rmx',
  max_mx_a_records = 5, -- Maximum number of A records to check per MX request
  wait_for_greeting = false, -- Wait for SMTP greeting and emit `quit` command
}
local redis_params
local exclude_domains

local E = {}
local CRLF = '\r\n'
local mx_miss_cache_prefix = 'mx_miss:'

local function mx_check(task)
  local ip_addr = task:get_ip()
  if task:get_user() or (ip_addr and ip_addr:is_local()) then
    return
  end

  local from = task:get_from('smtp')
  local mx_domain
  if ((from or E)[1] or E).domain and not from[2] then
    mx_domain = from[1]['domain']
  else
    mx_domain = task:get_helo()

    if mx_domain then
      mx_domain = rspamd_util.get_tld(mx_domain)
    end
  end

  if not mx_domain then
    return
  end

  if exclude_domains then
    if exclude_domains:get_key(mx_domain) then
      rspamd_logger.infox(task, 'skip mx check for %s, excluded', mx_domain)
      task:insert_result(settings.symbol_white_mx, 1.0, mx_domain)
      return
    end
  end

  local valid = false

  local function check_results(mxes)
    if fun.all(function(_, elt)
      return elt.checked
    end, mxes) then
      -- Save cache
      local key = settings.key_prefix .. mx_domain
      local function redis_cache_cb(err)
        if err ~= nil then
          rspamd_logger.errx(task, 'redis_cache_cb received error: %1', err)
          return
        end
      end
      if not valid then
        -- Greylist message
        if settings.greylist_invalid then
          task:get_mempool():set_variable("grey_greylisted_required", "1")
          lua_util.debugm(N, task, "advice to greylist a message")
          task:insert_result(settings.symbol_bad_mx, 1.0, "greylisted")
        else
          task:insert_result(settings.symbol_bad_mx, 1.0)
        end
        local ret = rspamd_redis_make_request(task,
            redis_params, -- connect params
            key, -- hash key
            true, -- is write
            redis_cache_cb, --callback
            'SETEX', -- command
            { key, tostring(settings.expire_novalid), '0' } -- arguments
        )
        lua_util.debugm(N, task, "set redis cache key: %s; invalid MX", key)
        if not ret then
          rspamd_logger.errx(task, 'got error connecting to redis')
        end
      else
        local valid_mx = {}
        fun.each(function(k)
          table.insert(valid_mx, k)
        end, fun.filter(function(_, elt)
          return elt.working
        end, mxes))
        task:insert_result(settings.symbol_good_mx, 1.0, valid_mx)
        local value = table.concat(valid_mx, ';')
        if mxes[mx_domain] and type(mxes[mx_domain]) == 'table' and mxes[mx_domain].mx_missing then
          value = mx_miss_cache_prefix .. value
        end
        local ret = rspamd_redis_make_request(task,
            redis_params, -- connect params
            key, -- hash key
            true, -- is write
            redis_cache_cb, --callback
            'SETEX', -- command
            { key, tostring(settings.expire), value } -- arguments
        )
        lua_util.debugm(N, task, "set redis cache key: %s; %s", key, value)
        if not ret then
          rspamd_logger.errx(task, 'error connecting to redis')
        end
      end
    end
  end

  local function gen_mx_a_callback(name, mxes)
    return function(_, _, results, err)
      lua_util.debugm(N, task, "got DNS results for %s: %s", name, results)
      mxes[name].ips = results

      local function io_cb(io_err, _, conn)
        lua_util.debugm(N, task, "TCP IO callback for %s, error: %s", name, io_err)
        if io_err then
          mxes[name].checked = true
          conn:close()
        else
          mxes[name].checked = true
          mxes[name].working = true
          valid = true
          if settings.wait_for_greeting then
            conn:add_write(function(_)
              conn:close()
            end, string.format('QUIT%s', CRLF))
          end
        end
        check_results(mxes)
      end
      local function on_connect_cb(conn)
        lua_util.debugm(N, task, "TCP connect callback for %s, error: %s", name, err)
        if err then
          mxes[name].checked = true
          conn:close()
          check_results(mxes)
        else
          mxes[name].checked = true
          valid = true
          mxes[name].working = true
        end

        -- Disconnect without SMTP dialog
        if not settings.wait_for_greeting then
          check_results(mxes)
          conn:close()
        end
      end

      if err or not results or #results == 0 then
        mxes[name].checked = true
      else
        -- Try to open TCP connection to port 25 for a random IP address
        -- see #3839 on GitHub
        lua_util.shuffle(results)
        local str_ip = results[1]:to_string()
        lua_util.debugm(N, task, "trying to connect to IP %s", str_ip)
        local t_ret = rspamd_tcp.new({
          task = task,
          host = str_ip,
          callback = io_cb,
          stop_pattern = CRLF,
          on_connect = on_connect_cb,
          timeout = settings.timeout,
          port = 25
        })

        if not t_ret then
          mxes[name].checked = true
        end
      end
      check_results(mxes)
    end
  end

  local function mx_callback(_, _, results, err)
    local mxes = {}
    if err or not results then
      local r = task:get_resolver()
      -- XXX: maybe add ipv6?
      -- fallback to implicit mx
      if not err and not results then
        err = 'no MX records found'
      end

      lua_util.debugm(N, task, "cannot find MX record for %s: %s, use implicit fallback",
          mx_domain, err)
      mxes[mx_domain] = { checked = false, working = false, ips = {}, mx_missing = true }
      r:resolve('a', {
        name = mx_domain,
        callback = gen_mx_a_callback(mx_domain, mxes),
        task = task,
        forced = true
      })
      task:insert_result(settings.symbol_no_mx, 1.0, err)
    else
      -- Inverse sort by priority
      table.sort(results, function(r1, r2)
        return r1['priority'] > r2['priority']
      end)

      local max_mx_to_resolve = math.min(#results, settings.max_mx_a_records)
      lua_util.debugm(N, task, 'check %s MX records (%d actually returned)',
          max_mx_to_resolve, #results)
      for i = 1, max_mx_to_resolve do
        local mx = results[i]
        mxes[mx.name] = { checked = false, working = false, ips = {} }
        local r = task:get_resolver()
        -- XXX: maybe add ipv6?
        r:resolve('a', {
          name = mx.name,
          callback = gen_mx_a_callback(mx.name, mxes),
          task = task,
          forced = true
        })
      end
      check_results(mxes)
    end
  end

  if not redis_params then
    local r = task:get_resolver()
    r:resolve('mx', {
      name = mx_domain,
      callback = mx_callback,
      task = task,
      forced = true
    })
  else
    local function redis_cache_get_cb(err, data)
      if err or type(data) ~= 'string' then
        local r = task:get_resolver()
        r:resolve('mx', {
          name = mx_domain,
          callback = mx_callback,
          task = task,
          forced = true
        })
      else
        if data == '0' then
          task:insert_result(settings.symbol_bad_mx, 1.0, 'cached')
        else
          if lua_util.str_startswith(data, mx_miss_cache_prefix) then
            task:insert_result(settings.symbol_no_mx, 1.0, 'cached')
            data = string.sub(data, #mx_miss_cache_prefix + 1)
          end
          local mxes = lua_util.str_split(data, ';')
          task:insert_result(settings.symbol_good_mx, 1.0, 'cached: ' .. mxes[1])
        end
      end
    end

    local key = settings.key_prefix .. mx_domain
    local ret = rspamd_redis_make_request(task,
        redis_params, -- connect params
        key, -- hash key
        false, -- is write
        redis_cache_get_cb, --callback
        'GET', -- command
        { key } -- arguments
    )

    if not ret then
      local r = task:get_resolver()
      r:resolve('mx', {
        name = mx_domain,
        callback = mx_callback,
        task = task,
        forced = true
      })
    end
  end
end

-- Module setup
local opts = rspamd_config:get_all_opt('mx_check')
if not (opts and type(opts) == 'table') then
  rspamd_logger.infox(rspamd_config, 'module is unconfigured')
  return
end
if opts then
  redis_params = lua_redis.parse_redis_server('mx_check')
  if not redis_params then
    rspamd_logger.errx(rspamd_config, 'no redis servers are specified, disabling module')
    lua_util.disable_module(N, "redis")
    return
  end

  settings = lua_util.override_defaults(settings, opts)
  lua_redis.register_prefix(settings.key_prefix .. '*', N,
      'MX check cache', {
        type = 'string',
      })

  local id = rspamd_config:register_symbol({
    name = settings.symbol_bad_mx,
    type = 'normal',
    callback = mx_check,
    flags = 'empty',
    augmentations = { string.format("timeout=%f", settings.timeout + rspamd_config:get_dns_timeout() or 0.0) },
  })
  rspamd_config:register_symbol({
    name = settings.symbol_no_mx,
    type = 'virtual',
    parent = id
  })
  rspamd_config:register_symbol({
    name = settings.symbol_good_mx,
    type = 'virtual',
    parent = id
  })
  rspamd_config:register_symbol({
    name = settings.symbol_white_mx,
    type = 'virtual',
    parent = id
  })

  rspamd_config:set_metric_symbol({
    name = settings.symbol_bad_mx,
    score = 0.5,
    description = 'Domain has no working MX',
    group = 'MX',
    one_shot = true,
    one_param = true,
  })
  rspamd_config:set_metric_symbol({
    name = settings.symbol_good_mx,
    score = -0.01,
    description = 'Domain has working MX',
    group = 'MX',
    one_shot = true,
    one_param = true,
  })
  rspamd_config:set_metric_symbol({
    name = settings.symbol_white_mx,
    score = 0.0,
    description = 'Domain is whitelisted from MX check',
    group = 'MX',
    one_shot = true,
    one_param = true,
  })
  rspamd_config:set_metric_symbol({
    name = settings.symbol_no_mx,
    score = 3.5,
    description = 'Domain has no resolvable MX',
    group = 'MX',
    one_shot = true,
    one_param = true,
  })

  if settings.exclude_domains then
    exclude_domains = rspamd_config:add_map {
      type = 'set',
      description = 'Exclude specific domains from MX checks',
      url = settings.exclude_domains,
    }
  end
end
