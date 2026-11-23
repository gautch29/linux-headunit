// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/lua_host.hpp"
#include "headunit/binary_plist.hpp"
#include <array>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("Lua host test failed");
}
headunit::LuaHost::Bytes bytes(const std::string& value) {
  return {value.begin(), value.end()};
}
}  // namespace
int main() {
  const std::map<std::string, std::string> resources{
      {"Fixture/Module.lua", "return {value=42}"},
      {"Fixture/Nested.lua", "return {module=require('Fixture.Module')}"},
      {"Fixture/Cycle.lua", "return require('Fixture.Cycle')"},
      {"raw.bin", "abc"}};
  const auto reader = [&](const std::string& path) {
    const auto item = resources.find(path);
    if (item == resources.end()) throw std::invalid_argument("unknown fixture resource");
    return bytes(item->second);
  };
  headunit::LuaHost host(reader);
  host.connected(true);
  host.execute(R"(
    assert(io == nil and os == nil and dofile == nil and loadfile == nil and load == nil)
    assert(package == nil and collectgarbage == nil)
    assert(debug.getregistry == nil and debug.info ~= nil)
    assert(loadstring('return 6*7')()==42)
    local bad,err=loadstring('this is invalid!'); assert(bad==nil and type(err)=='string')
    assert(not pcall(loadstring,string.rep(' ',65537)))
    assert(debug.sethook == nil and debug.getupvalue == nil)
    assert(require('Fixture.Module').value == 42)
    assert(require('Fixture/Module') == require('Fixture.Module'))
    assert(require('Fixture.Nested').module == require('Fixture.Module'))
    local ok, err = pcall(require, 'Fixture.Cycle')
    assert(not ok and string.find(err, 'cyclic module'))
    assert(not pcall(function() debug.info = nil end))
    assert(not pcall(function() CP.sendMessage = nil end))
    local sum: number = 0
    for i=1,3 do if i == 2 then continue end; sum += i end
    assert(sum == 4)
    assert(#CP.createAssetRAW('raw.bin') == 3)
    assert(CP.getSystemUpTime() == 0)
    assert(#CP.enumerateOutputStreams() == 0)
    assert(CP.getLogLevel() == CP.LOGLEVELS.INFO)
    CP.log(CP.LOGLEVELS.INFO, 'test', 'original synthetic fixture')
    local readable, writable, err = CP.status()
    assert(not readable and writable and err == nil)
    task = CP.DPLuaTask.create()
    task:run(function()
      assert(CP.getSystemUpTime() == 1)
      assert(CP.sendMessage('first'))
      coroutine.yield()
      local value, err = CP.receiveMessage()
      assert(value and #value == 3 and err == nil)
      assert(CP.sendMessage(value))
      task:stop()
      task = nil
      -- Host owns collection; scripts cannot force it.
    end)
    assert(task:isBusy())
  )");
  check(host.active_tasks() == 1);
  host.tick(1);
  check(host.take_outbound() == std::vector<headunit::LuaHost::Bytes>{bytes("first")});
  const std::array<std::uint8_t, 3> incoming{0, 42, 255};
  check(host.receive(incoming));
  host.tick(2);
  check(host.active_tasks() == 0);
  check(host.take_outbound() == std::vector<headunit::LuaHost::Bytes>{{0, 42, 255}});
  check(host.take_logs().size() == 1);
  host.execute(R"(
    first = CP.DPLuaTask.create()
    second = CP.DPLuaTask.create()
    first:run(function() CP.sendMessage('a'); coroutine.yield(); CP.sendMessage('b') end)
    second:run(function() CP.sendMessage('c'); coroutine.yield(); CP.sendMessage('d') end)
  )");
  // A task must remain alive only while referenced by its owner or coroutine.
  host.tick(3);
  auto first = host.take_outbound();
  check(first.size() == 2 && first[0] == bytes("a") && first[1] == bytes("c"));
  host.tick(4);
  check(host.take_outbound() == std::vector<headunit::LuaHost::Bytes>{bytes("b"), bytes("d")});
  host.execute("CP.reset(); local r,w,e=CP.status(); assert(not r and not w and e=='closed')");
  check(host.reset_count() == 1 && !host.receive(incoming));
  host.connected(true);
  for (unsigned i = 0; i < 64; ++i)
    check(host.receive(incoming));
  check(!host.receive(incoming));
  host.connected(false);
  check(host.take_outbound().empty());
  bool clock_rejected = false;
  try {
    host.tick(3);
  } catch (const std::invalid_argument&) {
    clock_rejected = true;
  }
  check(clock_rejected);
  {
    headunit::LuaHost invalid(reader);
    bool rejected = false;
    try {
      invalid.execute("CP.UnimplementedTestApi()");
    } catch (const std::runtime_error& error) {
      rejected = std::string(error.what()).find("unsupported CP API") != std::string::npos;
    }
    check(rejected);
  }
  {
    headunit::LuaHost infinite(reader);
    bool stopped = false;
    try {
      infinite.execute("while true do end");
    } catch (const std::runtime_error& error) {
      stopped = std::string(error.what()).find("execution budget") != std::string::npos;
    }
    check(stopped);
  }
  {
    headunit::LuaHost infinite(reader);
    infinite.execute("task=CP.DPLuaTask.create(); task:run(function() while true do end end)");
    bool stopped = false;
    try {
      infinite.tick(0);
    } catch (const std::runtime_error&) {
      stopped = true;
    }
    check(stopped);
  }
  {
    headunit::LuaHost limited(reader, 1024 * 1024);
    bool stopped = false;
    try {
      limited.execute("s=string.rep('a', 2000000)");
    } catch (const std::runtime_error&) {
      stopped = true;
    }
    check(stopped && limited.memory_used() <= 1024 * 1024);
  }
  {
    headunit::LuaHost queues(reader);
    queues.connected(true);
    queues.execute(R"(
      for i=1,64 do assert(CP.sendMessage('small')) end
      local ok, err=CP.sendMessage('overflow'); assert(ok==nil and err=='timeout')
    )");
    check(queues.take_outbound().size() == 64);
    queues.execute("assert(CP.sendMessage('recovered'))");
    check(queues.take_outbound().size() == 1);
  }
  {
    headunit::LuaHost replacement(reader);
    replacement.connected(true);
    replacement.execute(R"(
      task = CP.DPLuaTask.create()
      task:run(function()
        task:run(function() CP.sendMessage('replacement') end)
      end)
    )");
    replacement.tick(1);
    check(replacement.active_tasks() == 1 && replacement.take_outbound().empty());
    replacement.tick(2);
    check(replacement.active_tasks() == 0);
    check(replacement.take_outbound() ==
          std::vector<headunit::LuaHost::Bytes>{bytes("replacement")});
  }
  {
    headunit::LuaHost yields(reader);
    yields.connected(true);
    yields.execute(R"(
      task = CP.DPLuaTask.create()
      task:run(function()
        local result = coroutine.yield(1, 2, 3)
        assert(result == nil)
        CP.sendMessage('resumed')
        return 1, 2, 3
      end)
    )");
    yields.tick(1);
    yields.tick(2);
    check(yields.active_tasks() == 0);
    check(yields.take_outbound() == std::vector<headunit::LuaHost::Bytes>{bytes("resumed")});
  }
  {
    headunit::LuaHost plists(reader);
    plists.connected(true);
    plists.execute(R"(
      local value = {flag=false, number=-123, real=1.25, label='été 🗺', list={1,2,3}, nested={enabled=true}}
      local encoded=CP.createPlist(value)
      local result=CP.parsePlist(encoded)
      assert(result.flag==false and result.number==-123 and result.real==1.25)
      assert(result.label==value.label and #result.list==3 and result.nested.enabled)
      local inner=CP.createPlist({answer=42})
      local outer=CP.parsePlist(CP.createPlist({data=inner, text='plain'}))
      assert(type(outer.data)=='userdata' and type(outer.text)=='string')
      assert(CP.parsePlist(outer.data).answer==42)
      local bad,err=CP.parsePlist('not a plist'); assert(bad==nil and err=='invalid plist')
      local cycle={}; cycle.self=cycle
      assert(not pcall(CP.createPlist, cycle))
      assert(not pcall(CP.createPlist, {[1]=1,[3]=3}))
      assert(not pcall(CP.createPlist, {[1]=1,name='mixed'}))
      assert(not pcall(CP.createPlist, {value=0/0}))
      assert(not pcall(CP.createPlist, {value=9007199254740992}))
      assert(not pcall(CP.createPlist, {value=function() end}))
      assert(CP.sendMessage(CP.createPlist({stillWorks=true})))
    )");
    const auto emitted = plists.take_outbound();
    check(emitted.size() == 1);
    using V = headunit::PlistValue;
    check(headunit::decode_binary_plist(emitted[0]) == V{V::Dictionary{{"stillWorks", V{true}}}});
    const auto arrays = headunit::encode_binary_plist(V{V::Dictionary{
        {"array", V{V::Array{}}}, {"dictionary", V{V::Dictionary{}}}, {"data", V{V::Bytes{}}}}});
    check(plists.receive(arrays));
    plists.execute(
        "local v=CP.parsePlist(CP.receiveMessage()); assert(CP.sendMessage(CP.createPlist(v)))");
    check(headunit::decode_binary_plist(plists.take_outbound().at(0)) ==
          headunit::decode_binary_plist(arrays));
  }
  {
    headunit::LuaHost display(reader, 64 * 1024 * 1024,
                              {{"Fixture_Display", 1280, 480, 1.0 / 60.0}});
    display.execute(R"(
      local outputs=CP.enumerateOutputStreams()
      assert(#outputs==1 and outputs[1].identifier=='Fixture_Display')
      assert(outputs[1].width==1280 and outputs[1].height==480 and outputs[1].frameDuration==1/60)
      outputs[1].width=1
      assert(CP.enumerateOutputStreams()[1].width==1280)
    )");
  }
  return 0;
}
