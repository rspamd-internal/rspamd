context("Base64 encoding", function()
  local ffi = require("ffi")
  local util = require("rspamd_util")
  ffi.cdef[[
    void rspamd_cryptobox_init (void);
    void ottery_rand_bytes(void *buf, size_t n);
    unsigned ottery_rand_unsigned(void);
    unsigned char* g_base64_decode (const char *in, size_t *outlen);
    char * rspamd_encode_base64 (const unsigned char *in, size_t inlen,
      size_t str_len, size_t *outlen);
    void g_free(void *ptr);
    int memcmp(const void *a1, const void *a2, size_t len);
    size_t base64_test (bool generic, size_t niters, size_t len);
    double rspamd_get_ticks (void);
  ]]

  ffi.C.rspamd_cryptobox_init()

  local function random_buf(max_size)
    local l = ffi.C.ottery_rand_unsigned() % max_size + 1
    local buf = ffi.new("unsigned char[?]", l)
    ffi.C.ottery_rand_bytes(buf, l)

    return buf, l
  end

  local function random_safe_buf(max_size)
    local l = ffi.C.ottery_rand_unsigned() % max_size + 1
    local buf = ffi.new("unsigned char[?]", l)

    for i = 0,l-1 do
      buf[i] = ffi.C.ottery_rand_unsigned() % 20 + string.byte('A')
    end

    buf[l - 1] = 0;

    return buf, l
  end

  test("Base64 encode test", function()
    local cases = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
    }

    local nl = ffi.new("size_t [1]")
    for _,c in ipairs(cases) do
      local b = ffi.C.rspamd_encode_base64(c[1], #c[1], 0, nl)
      local s = ffi.string(b)
      ffi.C.g_free(b)
      assert_equal(s, c[2], s .. " not equal " .. c[2])
    end
  end)

  test("Base64 decode test", function()
    local cases = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
    }

    for _,c in ipairs(cases) do
      local b = tostring(util.decode_base64(c[2]))
      assert_equal(b, c[1], b .. " not equal " .. c[1])
    end
  end)

  test("Base64 line split encode test", function()
    local text = [[
Man is distinguished, not only by his reason, but by this singular passion from
other animals, which is a lust of the mind, that by a perseverance of delight
in the continued and indefatigable generation of knowledge, exceeds the short
vehemence of any carnal pleasure.]]
    local b64 = "TWFuIGlzIGRpc3Rpbmd1aXNoZWQsIG5vdCBvbmx5IGJ5IGhpcyByZWFzb24sIGJ1dCBieSB0aGlz\r\nIHNpbmd1bGFyIHBhc3Npb24gZnJvbQpvdGhlciBhbmltYWxzLCB3aGljaCBpcyBhIGx1c3Qgb2Yg\r\ndGhlIG1pbmQsIHRoYXQgYnkgYSBwZXJzZXZlcmFuY2Ugb2YgZGVsaWdodAppbiB0aGUgY29udGlu\r\ndWVkIGFuZCBpbmRlZmF0aWdhYmxlIGdlbmVyYXRpb24gb2Yga25vd2xlZGdlLCBleGNlZWRzIHRo\r\nZSBzaG9ydAp2ZWhlbWVuY2Ugb2YgYW55IGNhcm5hbCBwbGVhc3VyZS4="
    local nl = ffi.new("size_t [1]")
    local b = ffi.C.rspamd_encode_base64(text, #text, 76, nl)
    local cmp = ffi.C.memcmp(b, b64, nl[0])
    ffi.C.g_free(b)
    assert_equal(cmp, 0)
  end)

  test("Base64 fuzz test", function()
    for i = 1,1000 do
      local b, l = random_safe_buf(4096)
      local lim = ffi.C.ottery_rand_unsigned() % 64 + 10
      local orig = ffi.string(b)
      local ben = util.encode_base64(orig, lim)
      local dec = util.decode_base64(ben)
      assert_equal(orig, tostring(dec), "fuzz test failed for length: " .. #orig)
    end
  end)
    test("Base64 fuzz test (ffi)", function()
    for i = 1,1000 do
      local b, l = random_buf(4096)
      local nl = ffi.new("size_t [1]")
      local lim = ffi.C.ottery_rand_unsigned() % 64 + 10
      local ben = ffi.C.rspamd_encode_base64(b, l, lim, nl)
      local bs = ffi.string(ben)
      local ol = ffi.new("size_t [1]")
      local nb = ffi.C.g_base64_decode(ben, ol)

      local cmp = ffi.C.memcmp(b, nb, l)
      ffi.C.g_free(ben)
      ffi.C.g_free(nb)
      assert_equal(cmp, 0, "fuzz test failed for length: " .. tostring(l))
    end
  end)

  local speed_iters = 10000

  test("Base64 test reference vectors 1K", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(true, speed_iters, 1024)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Reference base64 (1K): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
  test("Base64 test optimized vectors 1K", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(false, speed_iters, 1024)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Optimized base64 (1K): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
    test("Base64 test reference vectors 512", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(true, speed_iters, 512)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Reference base64 (512): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
  test("Base64 test optimized vectors 512", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(false, speed_iters, 512)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Optimized base64 (512): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
    test("Base64 test reference vectors 10K", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(true, speed_iters / 100, 10240)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Reference base64 (10K): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
  test("Base64 test optimized vectors 10K", function()
    local t1 = ffi.C.rspamd_get_ticks()
    local res = ffi.C.base64_test(false, speed_iters / 100, 10240)
    local t2 = ffi.C.rspamd_get_ticks()

    print("Optimized base64 (10K): " .. tostring(t2 - t1) .. " sec")
    assert_not_equal(res, 0)
  end)
end)
