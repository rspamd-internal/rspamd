context("Rspamd util for lua - check generic functions", function()
    local util  = require 'rspamd_util'

    local cases = {
        {
            input = "test1",
            result = false,
            mixed_script = false,
            range_start = 0x0000,
            range_end = 0x017f
        },
        {
            input = "test test xxx",
            result = false,
            mixed_script = false,
            range_start = 0x0000,
            range_end = 0x017f
        },
        {
            input = "АбЫрвАлг",
            result = true,
            mixed_script = false,
            range_start = 0x0000,
            range_end = 0x017f
        },
        {
            input = "АбЫрвАлг example",
            result = true,
            mixed_script = true,
            range_start = 0x0000,
            range_end = 0x017f
        },
        {
            input = "example ąłśćżłóę",
            result = false,
            mixed_script = false,
            range_start = 0x0000,
            range_end = 0x017f
        },
        {
            input = "ąłśćżłóę АбЫрвАлг",
            result = true,
            mixed_script = true,
            range_start = 0x0000,
            range_end = 0x017f
        },
    }

    for i,c in ipairs(cases) do
        test("is_utf_outside_range, test case #" .. i, function()
          local actual = util.is_utf_outside_range(c.input, c.range_start, c.range_end)

          assert_equal(c.result, actual)
        end)
    end

    test("is_utf_outside_range, check cache", function ()
        cache_size = 20
        for i = 1,cache_size do
            local res = util.is_utf_outside_range("a", 0x0000, 0x0000+i)
        end
    end)

    test("is_utf_outside_range, check empty string", function ()
        assert_error(util.is_utf_outside_range)
    end)

    test("get_string_stats, test case", function()
        local res = util.get_string_stats("this is test 99")
        assert_equal(res["letters"], 10)
        assert_equal(res["digits"], 2)
    end)

    for i,c in ipairs(cases) do
        test("is_utf_mixed_script, test case #" .. i, function()
          local actual = util.is_utf_mixed_script(c.input)

          assert_equal(c.mixed_script, actual)
        end)
    end

    test("is_utf_mixed_script, invalid utf str should return errror", function()
        assert_error(util.is_utf_mixed_script,'\200\213\202')
    end)

    test("is_utf_mixed_script, empty str should return errror", function()
        assert_error(util.is_utf_mixed_script,'\200\213\202')
    end)
end)
