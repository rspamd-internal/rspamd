--[[
Copyright (c) 2016, Vsevolod Stakhov <vsevolod@highsecure.ru>

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

local rspamd_logger = require 'rspamd_logger'
local rspamd_lua_utils = require "lua_util"
local upstream_list = require "rspamd_upstream_list"
local lua_util = require "lua_util"
local lua_clickhouse = require "lua_clickhouse"
local fun = require "fun"

local N = "clickhouse"

if confighelp then
  return
end

local data_rows = {}
local custom_rows = {}
local nrows = 0
local schema_version = 5 -- Current schema version

local settings = {
  limit = 1000,
  timeout = 5.0,
  bayes_spam_symbols = {'BAYES_SPAM'},
  bayes_ham_symbols = {'BAYES_HAM'},
  ann_spam_symbols = {'NEURAL_SPAM'},
  ann_ham_symbols = {'NEURAL_HAM'},
  fuzzy_symbols = {'FUZZY_DENIED'},
  whitelist_symbols = {'WHITELIST_DKIM', 'WHITELIST_SPF_DKIM', 'WHITELIST_DMARC'},
  dkim_allow_symbols = {'R_DKIM_ALLOW'},
  dkim_reject_symbols = {'R_DKIM_REJECT'},
  dkim_dnsfail_symbols = {'R_DKIM_TEMPFAIL', 'R_DKIM_PERMFAIL'},
  dkim_na_symbols = {'R_DKIM_NA'},
  dmarc_allow_symbols = {'DMARC_POLICY_ALLOW'},
  dmarc_reject_symbols = {'DMARC_POLICY_REJECT'},
  dmarc_quarantine_symbols = {'DMARC_POLICY_QUARANTINE'},
  dmarc_softfail_symbols = {'DMARC_POLICY_SOFTFAIL'},
  dmarc_na_symbols = {'DMARC_NA'},
  spf_allow_symbols = {'R_SPF_ALLOW'},
  spf_reject_symbols = {'R_SPF_FAIL'},
  spf_dnsfail_symbols = {'R_SPF_DNSFAIL', 'R_SPF_PERMFAIL'},
  spf_neutral_symbols = {'R_DKIM_TEMPFAIL', 'R_DKIM_PERMFAIL'},
  spf_na_symbols = {'R_SPF_NA'},
  stop_symbols = {},
  ipmask = 19,
  ipmask6 = 48,
  full_urls = false,
  from_tables = nil,
  enable_symbols = false,
  database = 'default',
  use_https = false,
  use_gzip = true,
  allow_local = false,
  insert_subject = false,
  subject_privacy = false, -- subject privacy is off
  subject_privacy_alg = 'blake2', -- default hash-algorithm to obfuscate subject
  subject_privacy_prefix = 'obf', -- prefix to show it's obfuscated
  subject_privacy_length = 16, -- cut the length of the hash
  schema_additions = {}, -- additional SQL statements to be executed when schema is uploaded
  user = nil,
  password = nil,
  no_ssl_verify = false,
  custom_rules = {},
  enable_digest = false,
  retention = {
    enable = false,
    method = 'detach',
    period_months = 3,
    run_every = '7d',
  }
}

--- @language SQL
local clickhouse_schema = {[[
CREATE TABLE rspamd
(
    Date Date,
    TS DateTime,
    From String,
    MimeFrom String,
    IP String,
    Score Float32,
    NRcpt UInt8,
    Size UInt32,
    IsWhitelist Enum8('blacklist' = 0, 'whitelist' = 1, 'unknown' = 2) DEFAULT 'unknown',
    IsBayes Enum8('ham' = 0, 'spam' = 1, 'unknown' = 2) DEFAULT 'unknown',
    IsFuzzy Enum8('whitelist' = 0, 'deny' = 1, 'unknown' = 2) DEFAULT 'unknown',
    IsFann Enum8('ham' = 0, 'spam' = 1, 'unknown' = 2) DEFAULT 'unknown',
    IsDkim Enum8('reject' = 0, 'allow' = 1, 'unknown' = 2, 'dnsfail' = 3, 'na' = 4) DEFAULT 'unknown',
    IsDmarc Enum8('reject' = 0, 'allow' = 1, 'unknown' = 2, 'softfail' = 3, 'na' = 4, 'quarantine' = 5) DEFAULT 'unknown',
    IsSpf Enum8('reject' = 0, 'allow' = 1, 'neutral' = 2, 'dnsfail' = 3, 'na' = 4, 'unknown' = 5) DEFAULT 'unknown',
    NUrls Int32,
    Action Enum8('reject' = 0, 'rewrite subject' = 1, 'add header' = 2, 'greylist' = 3, 'no action' = 4, 'soft reject' = 5, 'custom' = 6) DEFAULT 'no action',
    CustomAction String,
    FromUser String,
    MimeUser String,
    RcptUser String,
    RcptDomain String,
    MimeRecipients Array(String),
    MessageId String,
    ListId String,
    Subject String,
    `Attachments.FileName` Array(String),
    `Attachments.ContentType` Array(String),
    `Attachments.Length` Array(UInt32),
    `Attachments.Digest` Array(FixedString(16)),
    `Urls.Tld` Array(String),
    `Urls.Url` Array(String),
    Emails Array(String),
    ASN String,
    Country FixedString(2),
    IPNet String,
    `Symbols.Names` Array(String),
    `Symbols.Scores` Array(Float64),
    `Symbols.Options` Array(String),
    ScanTimeReal UInt32,
    ScanTimeVirtual UInt32,
    Digest FixedString(32),
    SMTPFrom ALIAS if(From = '', '', concat(FromUser, '@', From)),
    SMTPRcpt ALIAS if(RcptDomain = '', '', concat(RcptUser, '@', RcptDomain)),
    MIMEFrom ALIAS if(MimeFrom = '', '', concat(MimeUser, '@', MimeFrom)),
    MIMERcpt ALIAS MimeRecipients[1]
) ENGINE = MergeTree()
PARTITION BY toMonday(Date)
ORDER BY TS
]],
[[CREATE TABLE rspamd_version ( Version UInt32) ENGINE = TinyLog]],
[[INSERT INTO rspamd_version (Version) Values (${SCHEMA_VERSION})]],
}

-- This describes SQL queries to migrate between versions
local migrations = {
  [1] = {
    -- Move to a wide fat table
    [[ALTER TABLE rspamd
      ADD COLUMN `Attachments.FileName` Array(String) AFTER ListId,
      ADD COLUMN `Attachments.ContentType` Array(String) AFTER `Attachments.FileName`,
      ADD COLUMN `Attachments.Length` Array(UInt32) AFTER `Attachments.ContentType`,
      ADD COLUMN `Attachments.Digest` Array(FixedString(16)) AFTER `Attachments.Length`,
      ADD COLUMN `Urls.Tld` Array(String) AFTER `Attachments.Digest`,
      ADD COLUMN `Urls.Url` Array(String) AFTER `Urls.Tld`,
      ADD COLUMN Emails Array(String) AFTER `Urls.Url`,
      ADD COLUMN ASN String AFTER Emails,
      ADD COLUMN Country FixedString(2) AFTER ASN,
      ADD COLUMN IPNet String AFTER Country,
      ADD COLUMN `Symbols.Names` Array(String) AFTER IPNet,
      ADD COLUMN `Symbols.Scores` Array(Float64) AFTER `Symbols.Names`,
      ADD COLUMN `Symbols.Options` Array(String) AFTER `Symbols.Scores`]],
    -- Add explicit version
    [[CREATE TABLE rspamd_version ( Version UInt32) ENGINE = TinyLog]],
    [[INSERT INTO rspamd_version (Version) Values (2)]],
  },
  [2] = {
    -- Add `Subject` column
    [[ALTER TABLE rspamd
      ADD COLUMN Subject String AFTER ListId]],
    -- New version
    [[INSERT INTO rspamd_version (Version) Values (3)]],
  },
  [3] = {
    [[ALTER TABLE rspamd
      ADD COLUMN IsSpf Enum8('reject' = 0, 'allow' = 1, 'neutral' = 2, 'dnsfail' = 3, 'na' = 4, 'unknown' = 5) DEFAULT 'unknown' AFTER IsDmarc,
      MODIFY COLUMN IsDkim Enum8('reject' = 0, 'allow' = 1, 'unknown' = 2, 'dnsfail' = 3, 'na' = 4) DEFAULT 'unknown',
      MODIFY COLUMN IsDmarc Enum8('reject' = 0, 'allow' = 1, 'unknown' = 2, 'softfail' = 3, 'na' = 4, 'quarantine' = 5) DEFAULT 'unknown',
      ADD COLUMN MimeRecipients Array(String) AFTER RcptDomain,
      ADD COLUMN MessageId String AFTER MimeRecipients,
      ADD COLUMN ScanTimeReal UInt32 AFTER `Symbols.Options`,
      ADD COLUMN ScanTimeVirtual UInt32 AFTER ScanTimeReal]],
    -- Add aliases
    [[ALTER TABLE rspamd
      ADD COLUMN SMTPFrom ALIAS if(From = '', '', concat(FromUser, '@', From)),
      ADD COLUMN SMTPRcpt ALIAS if(RcptDomain = '', '', concat(RcptUser, '@', RcptDomain)),
      ADD COLUMN MIMEFrom ALIAS if(MimeFrom = '', '', concat(MimeUser, '@', MimeFrom)),
      ADD COLUMN MIMERcpt ALIAS MimeRecipients[1]
    ]],
    -- New version
    [[INSERT INTO rspamd_version (Version) Values (4)]],
  },
  [4] = {
    [[ALTER TABLE rspamd
      MODIFY COLUMN Action Enum8('reject' = 0, 'rewrite subject' = 1, 'add header' = 2, 'greylist' = 3, 'no action' = 4, 'soft reject' = 5, 'custom' = 6) DEFAULT 'no action',
      ADD COLUMN CustomAction String AFTER Action
    ]],
    -- New version
    [[INSERT INTO rspamd_version (Version) Values (5)]],
  },
}

local predefined_actions = {
  ['reject'] = true,
  ['rewrite subject'] = true,
  ['add header'] = true,
  ['greylist'] = true,
  ['no action'] = true,
  ['soft reject'] = true
}

local function clickhouse_main_row(res)
  local fields = {
    'Date',
    'TS',
    'From',
    'MimeFrom',
    'IP',
    'Score',
    'NRcpt',
    'Size',
    'IsWhitelist',
    'IsBayes',
    'IsFuzzy',
    'IsFann',
    'IsDkim',
    'IsDmarc',
    'NUrls',
    'Action',
    'FromUser',
    'MimeUser',
    'RcptUser',
    'RcptDomain',
    'ListId',
    'Subject',
    'Digest',
    -- 1.9.2 +
    'IsSpf',
    'MimeRecipients',
    'MessageId',
    'ScanTimeReal',
    'ScanTimeVirtual',
    -- 1.9.3 +
    'CustomAction',
  }

  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function clickhouse_attachments_row(res)
  local fields = {
    'Attachments.FileName',
    'Attachments.ContentType',
    'Attachments.Length',
    'Attachments.Digest',
  }

  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function clickhouse_urls_row(res)
  local fields = {
    'Urls.Tld',
    'Urls.Url',
  }
  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function clickhouse_emails_row(res)
  local fields = {
    'Emails',
  }
  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function clickhouse_symbols_row(res)
  local fields = {
    'Symbols.Names',
    'Symbols.Scores',
    'Symbols.Options',
  }
  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function clickhouse_asn_row(res)
  local fields = {
    'ASN',
    'Country',
    'IPNet',
  }
  for _,v in ipairs(fields) do table.insert(res, v) end
end

local function today(ts)
  return os.date('!%Y-%m-%d', ts)
end

local function clickhouse_check_symbol(task, settings_field_name, fields_table,
                                       field_name, value, value_negative)
  for _,s in ipairs(settings[settings_field_name] or {}) do
    if task:has_symbol(s) then
      if value_negative then
        local sym = task:get_symbol(s)[1]
        if sym['score'] > 0 then
          fields_table[field_name] = value
        else
          fields_table[field_name] = value_negative
        end
      else
        fields_table[field_name] = value
      end

      return true
    end
  end

  return false
end

local function clickhouse_send_data(task, ev_base)
  local log_object = task or rspamd_config
  local upstream = settings.upstream:get_upstream_round_robin()
  local ip_addr = upstream:get_addr():to_string(true)

  local function gen_success_cb(what, how_many)
    return function (_, _)
      rspamd_logger.infox(log_object, "sent %s rows of %s to clickhouse server %s",
          how_many, what, ip_addr)
      upstream:ok()
    end
  end

  local function gen_fail_cb(what, how_many)
    return function (_, err)
      rspamd_logger.errx(log_object, "cannot send %s rows of %s data to clickhouse server %s: %s",
          how_many, what, ip_addr, err)
      upstream:fail()
    end
  end

  local function send_data(what, tbl, query)
    local ch_params = {}
    if task then
      ch_params.task = task
    else
      ch_params.config = rspamd_config
      ch_params.ev_base = ev_base
    end

    local ret = lua_clickhouse.insert(upstream, settings, ch_params,
        query, tbl,
        gen_success_cb(what, #tbl),
        gen_fail_cb(what, #tbl))
    if not ret then
      rspamd_logger.errx(log_object, "cannot send %s rows of %s data to clickhouse server %s: %s",
          #tbl, what, ip_addr, 'cannot make HTTP request')
    end
  end

  local fields = {}
  clickhouse_main_row(fields)
  clickhouse_attachments_row(fields)
  clickhouse_urls_row(fields)
  clickhouse_emails_row(fields)
  clickhouse_asn_row(fields)

  if settings.enable_symbols then
    clickhouse_symbols_row(fields)
  end

  send_data('generic data', data_rows,
      string.format('INSERT INTO rspamd (%s)', table.concat(fields, ',')))

  for k,crows in pairs(custom_rows) do
    if #crows > 1 then
      send_data('custom data ('..k..')', settings.custom_rules[k].first_row(),
          crows)
    end
  end
end

local function clickhouse_collect(task)
  if task:has_flag('skip') then return end
  if not settings.allow_local and rspamd_lua_utils.is_rspamc_or_controller(task) then return end

  for _,sym in ipairs(settings.stop_symbols) do
    if task:has_symbol(sym) then
      lua_util.debugm(N, task, 'skip collection as symbol %s has fired', sym)
      return
    end
  end

  local from_domain = ''
  local from_user = ''
  if task:has_from('smtp') then
    local from = task:get_from('smtp')[1]

    if from then
      from_domain = from['domain']
      from_user = from['user']
    end

    if from_domain == '' then
      if task:get_helo() then
        from_domain = task:get_helo()
      end
    end
  else
    if task:get_helo() then
      from_domain = task:get_helo()
    end
  end

  local mime_domain = ''
  local mime_user = ''
  if task:has_from('mime') then
    local from = task:get_from({'mime','orig'})[1]
    if from then
      mime_domain = from['domain']
      mime_user = from['user']
    end
  end

  local mime_rcpt = {}
  if task:has_recipients('mime') then
    local from = task:get_recipients({'mime','orig'})
    mime_rcpt = fun.totable(fun.map(function (f) return f.addr or '' end, from))
  end

  local ip_str = 'undefined'
  local ip = task:get_from_ip()
  if ip and ip:is_valid() then
    local ipnet
    if ip:get_version() == 4 then
      ipnet = ip:apply_mask(settings['ipmask'])
    else
      ipnet = ip:apply_mask(settings['ipmask6'])
    end
    ip_str = ipnet:to_string()
  end

  local rcpt_user = ''
  local rcpt_domain = ''
  if task:has_recipients('smtp') then
    local rcpt = task:get_recipients('smtp')[1]
    rcpt_user = rcpt['user']
    rcpt_domain = rcpt['domain']
  end

  local list_id = task:get_header('List-Id') or ''
  local message_id = lua_util.maybe_obfuscate_string(task:get_message_id() or '',
      settings, 'mid')

  local score = task:get_metric_score('default')[1];
  local fields = {
    bayes = 'unknown',
    fuzzy = 'unknown',
    ann = 'unknown',
    whitelist = 'unknown',
    dkim = 'unknown',
    dmarc = 'unknown',
    spf = 'unknown',
  }

  local ret

  ret = clickhouse_check_symbol(task,'bayes_spam_symbols', fields,
      'bayes', 'spam')
  if not ret then
    clickhouse_check_symbol(task,'bayes_ham_symbols', fields,
        'bayes', 'ham')
  end

  clickhouse_check_symbol(task,'ann_symbols_spam', fields,
      'ann', 'spam')
  if not ret then
    clickhouse_check_symbol(task,'ann_symbols_ham', fields,
        'ann', 'ham')
  end

  clickhouse_check_symbol(task,'whitelist_symbols', fields,
      'whitelist', 'blacklist', 'whitelist')

  clickhouse_check_symbol(task,'fuzzy_symbols', fields,
      'fuzzy', 'deny')


  ret = clickhouse_check_symbol(task,'dkim_allow_symbols', fields,
      'dkim', 'allow')
  if not ret then
    ret = clickhouse_check_symbol(task,'dkim_reject_symbols', fields,
        'dkim', 'reject')
  end
  if not ret then
    ret = clickhouse_check_symbol(task,'dkim_dnsfail_symbols', fields,
        'dkim', 'dnsfail')
  end
  if not ret then
    clickhouse_check_symbol(task,'dkim_na_symbols', fields,
        'dkim', 'na')
  end


  ret = clickhouse_check_symbol(task,'dmarc_allow_symbols', fields,
      'dmarc', 'allow')
  if not ret then
    ret = clickhouse_check_symbol(task,'dmarc_reject_symbols', fields,
        'dmarc', 'reject')
  end
  if not ret then
    ret = clickhouse_check_symbol(task,'dmarc_quarantine_symbols', fields,
        'dmarc', 'quarantine')
  end
  if not ret then
    ret = clickhouse_check_symbol(task,'dmarc_softfail_symbols', fields,
        'dmarc', 'softfail')
  end
  if not ret then
    clickhouse_check_symbol(task,'dmarc_na_symbols', fields,
        'dmarc', 'na')
  end


  ret = clickhouse_check_symbol(task,'spf_allow_symbols', fields,
      'spf', 'allow')
  if not ret then
    ret = clickhouse_check_symbol(task,'spf_reject_symbols', fields,
        'spf', 'reject')
  end
  if not ret then
    ret = clickhouse_check_symbol(task,'spf_neutral_symbols', fields,
        'spf', 'neutral')
  end
  if not ret then
    ret = clickhouse_check_symbol(task,'spf_dnsfail_symbols', fields,
        'spf', 'dnsfail')
  end
  if not ret then
    clickhouse_check_symbol(task,'spf_na_symbols', fields,
        'spf', 'na')
  end

  local nrcpts = 0
  if task:has_recipients('smtp') then
    nrcpts = #task:get_recipients('smtp')
  end

  local nurls = 0
  if task:has_urls(true) then
    nurls = #task:get_urls(true)
  end

  local timestamp = math.floor(task:get_date({
    format = 'connect',
    gmt = true, -- The only sane way to sync stuff with different timezones
  }))

  local action = task:get_metric_action('default')
  local custom_action = ''

  if not predefined_actions[action] then
    custom_action = action
    action = 'custom'
  end

  local digest = ''

  if settings.enable_digest then
    digest = task:get_digest()
  end

  local subject = ''
  if settings.insert_subject then
    subject = lua_util.maybe_obfuscate_string(task:get_subject() or '', settings, 'subject')
  end

  local scan_real,scan_virtual = task:get_scan_time()
  scan_real,scan_virtual = math.floor(scan_real * 1000), math.floor(scan_virtual * 1000)
  if scan_real < 0 then
    rspamd_logger.messagex(task,
        'clock skew detected for message: %s ms real scan time (reset to 0), %s virtual scan time',
        scan_real, scan_virtual)
    scan_real = 0
  end

  local row = {
    today(timestamp),
    timestamp,
    from_domain,
    mime_domain,
    ip_str,
    score,
    nrcpts,
    task:get_size(),
    fields.whitelist,
    fields.bayes,
    fields.fuzzy,
    fields.ann,
    fields.dkim,
    fields.dmarc,
    nurls,
    action,
    from_user,
    mime_user,
    rcpt_user,
    rcpt_domain,
    list_id,
    subject,
    digest,
    fields.spf,
    mime_rcpt,
    message_id,
    scan_real,
    scan_virtual,
    custom_action
  }

  -- Attachments step
  local attachments_fnames = {}
  local attachments_ctypes = {}
  local attachments_lengths = {}
  local attachments_digests = {}
  for _,part in ipairs(task:get_parts()) do
    local fname = part:get_filename()

    if fname then
      table.insert(attachments_fnames, fname)
      local type, subtype = part:get_type()
      table.insert(attachments_ctypes, string.format("%s/%s",
          type, subtype))
      table.insert(attachments_lengths, part:get_length())
      table.insert(attachments_digests, string.sub(part:get_digest(), 1, 16))
    end
  end

  if #attachments_fnames > 0 then
    table.insert(row, attachments_fnames)
    table.insert(row,  attachments_ctypes)
    table.insert(row,  attachments_lengths)
    table.insert(row,   attachments_digests)
  else
    table.insert(row, {})
    table.insert(row, {})
    table.insert(row, {})
    table.insert(row, {})
  end

  local flatten_urls = function(f, ...)
    return fun.totable(fun.map(function(k,v) return f(k,v) end, ...))
  end

  -- Urls step
  local urls_urls = {}
  if task:has_urls(false) then

    for _,u in ipairs(task:get_urls(false)) do
      if settings['full_urls'] then
        urls_urls[u:get_text()] = u
      else
        urls_urls[u:get_host()] = u
      end
    end

    -- Get tlds
    table.insert(row, flatten_urls(function(_, u)
      return u:get_tld() or u:get_host()
    end, urls_urls))
    -- Get hosts/full urls
    table.insert(row, flatten_urls(function(k, _) return k end, urls_urls))
  else
    table.insert(row, {})
    table.insert(row, {})
  end

  -- Emails step
  if task:has_urls(true) then
    table.insert(row, flatten_urls(function(k, _) return k end,
        fun.map(function(u)
          return string.format('%s@%s', u:get_user(), u:get_host()),true
        end, task:get_emails())))
  else
    table.insert(row, {})
  end

  -- ASN information
  local asn, country, ipnet = '--', '--', '--'
  local pool = task:get_mempool()
  ret = pool:get_variable("asn")
  if ret then
    asn = ret
  end
  ret = pool:get_variable("country")
  if ret then
    country = ret:sub(1, 2)
  end
  ret = pool:get_variable("ipnet")
  if ret then
    ipnet = ret
  end
  table.insert(row, asn)
  table.insert(row, country)
  table.insert(row, ipnet)

  -- Symbols info
  if settings.enable_symbols then
    local symbols = task:get_symbols_all()
    local syms_tab = {}
    local scores_tab = {}
    local options_tab = {}

    for _,s in ipairs(symbols) do
      table.insert(syms_tab, s.name or '')
      table.insert(scores_tab, s.score)

      if s.options then
        table.insert(options_tab, table.concat(s.options, ','))
      else
        table.insert(options_tab, '');
      end
    end
    table.insert(row, syms_tab)
    table.insert(row, scores_tab)
    table.insert(row, options_tab)
  end

  -- Custom data
  for k,rule in pairs(settings.custom_rules) do
    if not custom_rows[k] then custom_rows[k] = {} end
    table.insert(custom_rows[k], rule.get_row(task))
  end

  nrows = nrows + 1
  table.insert(data_rows, row)
  lua_util.debugm(N, task, "add clickhouse row %s / %s", nrows, settings.limit)

  if nrows >= settings['limit'] then
    clickhouse_send_data(task)
    nrows = 0
    data_rows = {}
    custom_rows = {}
  end
end

local function do_remove_partition(ev_base, cfg, table_name, partition_id)
  lua_util.debugm(N, rspamd_config, "removing partition %s.%s", table_name, partition_id)
  local upstream = settings.upstream:get_upstream_round_robin()
  local remove_partition_sql = "ALTER TABLE ${table_name} ${remove_method} PARTITION ${partition_id}"
  local remove_method = (settings.retention.method == 'drop') and 'DROP' or 'DETACH'
  local sql_params = {
    ['table_name']     = table_name,
    ['remove_method']  = remove_method,
    ['partition_id']   = partition_id
  }

  local sql = rspamd_lua_utils.template(remove_partition_sql, sql_params)

  local ch_params = {
    body = sql,
    ev_base = ev_base,
    config = cfg,
  }

  local err, _ = lua_clickhouse.generic_sync(upstream, settings, ch_params, sql)
  if err then
    rspamd_logger.errx(rspamd_config,
      "cannot detach partition %s:%s from server %s: %s",
      table_name, partition_id,
      settings['server'], err)
    return
  end

  rspamd_logger.infox(rspamd_config,
      'detached partition %s:%s on server %s', table_name, partition_id,
      settings['server'])

end

--[[
  nil   - file is not writable, do not perform removal
  0     - it's time to perform removal
  <int> - how many seconds wait until next run
]]
local function get_last_removal_ago()
  local ts_file = string.format('%s/%s', rspamd_paths['DBDIR'], 'clickhouse_retention_run')
  local f, err = io.open(ts_file, 'r')
  local write_file
  local last_ts

  if err then
    lua_util.debugm(N, rspamd_config, 'Failed to open %s: %s', ts_file, err)
  else
    last_ts = tonumber(f:read('*number'))
    f:close()
  end

  local current_ts = os.time()

  if last_ts == nil or (last_ts + settings.retention.period) <= current_ts then
    write_file, err = io.open(ts_file, 'w')
    if err then
      rspamd_logger.errx(rspamd_config, 'Failed to open %s, will not perform retention: %s', ts_file, err)
      return nil
    end

    local res
    res, err = write_file:write(tostring(current_ts))
    if err or res == nil then
      rspamd_logger.errx(rspamd_config, 'Failed to write %s, will not perform retention: %s', ts_file, err)
      return nil
    end
    write_file:close()
    return 0
  end

  return (last_ts + settings.retention.period) - current_ts
end

local function clickhouse_remove_old_partitions(cfg, ev_base)
  local last_time_ago = get_last_removal_ago()
  if last_time_ago == nil then
    rspamd_logger.errx(rspamd_config, "Failed to get last run time. Disabling retention")
    return false
  elseif last_time_ago ~= 0 then
    return last_time_ago
  end

  local upstream = settings.upstream:get_upstream_round_robin()
  local partition_to_remove_sql = "SELECT distinct partition, table FROM system.parts WHERE " ..
      "table in ('${tables}') and max_date <= toDate(now() - interval ${month} month);"

  local table_names = {'rspamd'}
  local tables = table.concat(table_names, "', '")
  local sql_params = {
    tables = tables,
    month  = settings.retention.period_months,
  }
  local sql = rspamd_lua_utils.template(partition_to_remove_sql, sql_params)


  local ch_params = {
    ev_base = ev_base,
    config = cfg,
  }
  local err, rows = lua_clickhouse.select_sync(upstream, settings, ch_params, sql)
  if err then
    rspamd_logger.errx(rspamd_config,
      "cannot send data to clickhouse server %s: %s",
      settings['server'], err)
  else
    fun.each(function(row)
      do_remove_partition(ev_base, cfg, row.table, row.partition)
    end, rows)
  end

  -- settings.retention.period is added on initialisation, see below
  return settings.retention.period
end

local function upload_clickhouse_schema(upstream, ev_base, cfg)
  local ch_params = {
    ev_base = ev_base,
    config = cfg,
  }

  local errored = false

  -- Apply schema sequentially
  fun.each(function(v)
    if errored then
      rspamd_logger.errx(rspamd_config, "cannot upload schema '%s' on clickhouse server %s: due to previous errors",
          v, upstream:get_addr():to_string(true))
      return
    end
    local sql = v
    local err, reply = lua_clickhouse.generic_sync(upstream, settings, ch_params, sql)

    if err then
      rspamd_logger.errx(rspamd_config, "cannot upload schema '%s' on clickhouse server %s: %s",
        sql, upstream:get_addr():to_string(true), err)
      errored = true
      return
    end
    rspamd_logger.debugm(N, rspamd_config, 'uploaded clickhouse schema element %s to %s: %s',
        v, upstream:get_addr():to_string(true), reply)
  end,
      -- Also template schema version
      fun.map(function(v)
        return lua_util.template(v, {SCHEMA_VERSION = tostring(schema_version)})
      end, fun.chain(clickhouse_schema, settings.schema_additions)))
end

local function maybe_apply_migrations(upstream, ev_base, cfg, version)
  local ch_params = {
    ev_base = ev_base,
    config = cfg,
  }
  -- Apply migrations sequentially
  local function migration_recursor(i)
    if i < schema_version  then
      if migrations[i] then
        -- We also need to apply statements sequentially
        local function sql_recursor(j)
          if migrations[i][j] then
            local sql = migrations[i][j]
            local ret = lua_clickhouse.generic(upstream, settings, ch_params, sql,
                function(_, _)
                  rspamd_logger.infox(rspamd_config,
                      'applied migration to version %s from version %s: %s',
                      i + 1, version, sql:gsub('[\n%s]+', ' '))
                  if j == #migrations[i] then
                    -- Go to the next migration
                    migration_recursor(i + 1)
                  else
                    -- Apply the next statement
                    sql_recursor(j + 1)
                  end
                end ,
                function(_, err)
                  rspamd_logger.errx(rspamd_config,
                      "cannot apply migration %s: '%s' on clickhouse server %s: %s",
                      i, sql, upstream:get_addr():to_string(true), err)
                end)
            if not ret then
              rspamd_logger.errx(rspamd_config,
                  "cannot apply migration %s: '%s' on clickhouse server %s: cannot make request",
                  i, sql, upstream:get_addr():to_string(true))
            end
          end
        end

        sql_recursor(1)
      else
        -- Try another migration
        migration_recursor(i + 1)
      end
    end
  end

  migration_recursor(version)
end

local function check_rspamd_table(upstream, ev_base, cfg)
  local ch_params = {
    ev_base = ev_base,
    config = cfg,
  }
  local sql = [[EXISTS TABLE rspamd]]
  local err, rows = lua_clickhouse.select_sync(upstream, settings, ch_params, sql)
  if err then
    rspamd_logger.errx(rspamd_config, "cannot check rspamd table in clickhouse server %s: %s",
      upstream:get_addr():to_string(true), err)
    return
  end

  if rows[1] and rows[1].result then
    if tonumber(rows[1].result) == 1 then
      -- Apply migration
      rspamd_logger.infox(rspamd_config, 'table rspamd exists, apply migration')
      maybe_apply_migrations(upstream, ev_base, cfg, 1)
    else
      -- Upload schema
      rspamd_logger.infox(rspamd_config, 'table rspamd does not exists, upload full schema')
      upload_clickhouse_schema(upstream, ev_base, cfg)
    end
  else
    rspamd_logger.errx(rspamd_config,
        "unexpected reply on EXISTS command from server %s: %s",
        upstream:get_addr():to_string(true), rows)
  end
end


local function check_clickhouse_upstream(upstream, ev_base, cfg)
  local ch_params = {
    ev_base = ev_base,
    config = cfg,
  }
  -- If we have some custom rules, we just send its schema to the upstream
  for k,rule in pairs(settings.custom_rules) do
    if rule.schema then
      local sql = rspamd_lua_utils.template(rule.schema, settings)
      local err, _ = lua_clickhouse.generic_sync(upstream, settings, ch_params, sql)
      if err then
        rspamd_logger.errx(rspamd_config, 'cannot send custom schema %s to clickhouse server %s: ' ..
        'cannot make request (%s)',
            k, upstream:get_addr():to_string(true), err)
      end
    end
  end

  -- Now check the main schema and apply migrations if needed
  local sql = [[SELECT MAX(Version) as v FROM rspamd_version]]
  local err, rows = lua_clickhouse.select_sync(upstream, settings, ch_params, sql)
  if err then
    if rows and rows.code == 404 then
      rspamd_logger.infox(rspamd_config, 'table rspamd_version does not exist, check rspamd table')
      check_rspamd_table(upstream, ev_base, cfg)
    else
      rspamd_logger.errx(rspamd_config, "cannot get version on clickhouse server %s: %s",
        upstream:get_addr():to_string(true), err)
    end
  else
    local version = tonumber(rows[1].v)
    maybe_apply_migrations(upstream, ev_base, cfg, version)
  end
end

local opts = rspamd_config:get_all_opt('clickhouse')
if opts then
    for k,v in pairs(opts) do
      if k == 'custom_rules' then
        if not v[1] then
          v = {v}
        end

        for i,rule in ipairs(v) do
          if rule.schema and rule.first_row and rule.get_row then
            local first_row, get_row
            local loadstring = loadstring or load
            local ret, res_or_err = pcall(loadstring(rule.first_row))

            if not ret or type(res_or_err) ~= 'function' then
              rspamd_logger.errx(rspamd_config, 'invalid first_row (%s) - must be a function',
                  res_or_err)
            else
              first_row = res_or_err
            end

            ret, res_or_err = pcall(loadstring(rule.get_row))

            if not ret or type(res_or_err) ~= 'function' then
              rspamd_logger.errx(rspamd_config, 'invalid get_row (%s) - must be a function',
                  res_or_err)
            else
              get_row = res_or_err
            end

            if first_row and get_row then
              local name = rule.name or tostring(i)
              settings.custom_rules[name] = {
                schema = rule.schema,
                first_row = first_row,
                get_row = get_row,
              }
            end
          else
            rspamd_logger.errx(rspamd_config, 'custom rule has no required attributes: schema, first_row and get_row')
          end
        end
      else
        settings[k] = v
      end
    end

    if not settings['server'] and not settings['servers'] then
      rspamd_logger.infox(rspamd_config, 'no servers are specified, disabling module')
      rspamd_lua_utils.disable_module(N, "config")
    else
      settings['from_map'] = rspamd_map_add('clickhouse', 'from_tables',
        'regexp', 'clickhouse specific domains')

      settings.upstream = upstream_list.create(rspamd_config,
        settings['server'] or settings['servers'], 8123)

      if not settings.upstream then
        rspamd_logger.errx(rspamd_config, 'cannot parse clickhouse address: %s',
            settings['server'] or settings['servers'])
        rspamd_lua_utils.disable_module(N, "config")
        return
      end

      rspamd_config:register_symbol({
        name = 'CLICKHOUSE_COLLECT',
        type = 'idempotent',
        callback = clickhouse_collect,
        priority = 10,
        flags = 'empty,explicit_disable,ignore_passthrough',
      })
      rspamd_config:register_finish_script(function(task)
        if nrows > 0 then
          clickhouse_send_data(task, nil)
        end
      end)
      -- Create tables on load
      rspamd_config:add_on_load(function(cfg, ev_base, worker)
        if worker:is_primary_controller() then
          local upstreams = settings.upstream:all_upstreams()

          for _,up in ipairs(upstreams) do
            check_clickhouse_upstream(up, ev_base, cfg)
          end

          if settings.retention.enable and settings.retention.method ~= 'drop' and
              settings.retention.method ~= 'detach' then
            rspamd_logger.errx(rspamd_config,
                "retention.method should be either 'drop' or 'detach' (now: %s). Disabling retention",
                settings.retention.method)
            settings.retention.enable = false
          end
          if settings.retention.enable and settings.retention.period_months < 1 or
              settings.retention.period_months > 1000 then
            rspamd_logger.errx(rspamd_config,
                "please, set retention.period_months between 1 and 1000 (now: %s). Disabling retention",
                settings.retention.period_months)
            settings.retention.enable = false
          end
          local period = lua_util.parse_time_interval(settings.retention.run_every)
          if settings.retention.enable and period == nil then
            rspamd_logger.errx(rspamd_config, "invalid value for retention.run_every (%s). Disabling retention",
                    settings.retention.run_every)
            settings.retention.enable = false
          end

          if settings.retention.enable then
            settings.retention.period = period
            rspamd_logger.infox(rspamd_config,
                "retention will be performed each %s seconds for %s month with method %s",
                period, settings.retention.period_months, settings.retention.method)
            rspamd_config:add_periodic(ev_base, 0, clickhouse_remove_old_partitions, false)
          end
        end
      end)
    end
end
