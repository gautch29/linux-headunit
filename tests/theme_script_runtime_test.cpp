// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/script_channel.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <source_location>
#include <unistd.h>

using namespace headunit::theme;
namespace fs = std::filesystem;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn, std::source_location source = std::source_location::current()) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("expected rejection at line " + std::to_string(source.line()));
}
std::uint64_t thread_cpu_ns() {
  timespec value{};
  CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0);
  return std::uint64_t(value.tv_sec) * 1000000000ULL + std::uint64_t(value.tv_nsec);
}
struct Fixture {
  fs::path path;
  script::TrustedDirectory asset;
  Fixture() {
    char pattern[] = "/tmp/headunit-script-test-XXXXXX";
    const auto* p = ::mkdtemp(pattern);
    CHECK(p);
    path = p;
    asset.content_path = path;
    fs::create_directory(path / "Test");
    fs::create_directory(path / "r");
    write("r/PackageData.lua", R"(return {displays={{id='Cluster_Display',layouts={
      {id='layout-fixture',zones={{id='zone-fixture',type={dca={}},components={
        {id='map-fixture',name='DCA_Map',type='primary',slots={}}
      }}}},
      {id='ambiguous-layout',zones={{id='zone-a',type={dca={}},components={
        {id='map-a',name='DCA_Map',type='primary',slots={}}
      }},{id='zone-b',type={dca={}},components={
        {id='map-b',name='DCA_Map',type='primary',slots={}}
      }}}}
    }}}})");
    write("Test/Value.lua", "return {value = 42}");
    write("Test/Cycle.lua", "return require 'Test.Cycle'");
    write("raw.txt", "fixture");
    fs::create_symlink("/etc/passwd", path / "escape.txt");
    fs::create_symlink("/tmp", path / "outside");
  }
  ~Fixture() { fs::remove_all(path); }
  void write(const char* name, const char* text) { std::ofstream(path / name) << text; }
};
void modules_and_sandbox(const Fixture& f) {
  script::Runtime r(f.asset, {"main"});
  r.evaluate(R"(
    assert(os == nil and io == nil and package == nil and loadfile == nil)
    assert(loadstring('return 6*7')() == 42)
    local bad, reason = loadstring(string.char(5,1,0,255))
    assert(bad == nil and type(reason) == 'string')
    assert(require('Test.Value') == require('Test.Value'))
    assert(require('Test.Value').value == 42)
    assert(CP.createAssetRAW('raw.txt') == 'fixture')
    assert(CP.createCustomPNG('unavailable',0) == nil)
    assert(not pcall(CP.createAssetRAW, '../raw.txt'))
    assert(not pcall(CP.createAssetRAW, '/etc/passwd'))
    assert(not pcall(CP.createAssetRAW, 'escape.txt'))
    assert(not pcall(CP.createAssetRAW, 'outside/escape.txt'))
    assert(not pcall(require, 'Test.Cycle'))
    assert(not pcall(function() CP.version = 'changed' end))
    assert(not pcall(function() table.insert = false end))
    assert(CP.PTS == nil)
    local streams = CP.enumerateOutputStreams()
    assert(#streams == 1 and streams[1].identifier == 'main')
    local n = 0
    for _, x in {1,2,3} do if x == 2 then continue end n += x end
    assert(n == 4)
    local largeKeys = {[2^31]=1, [-2^31-1]=2, [1e100]=3, [math.huge]=4}
    assert(largeKeys[2^31] == 1 and largeKeys[-2^31-1] == 2)
    assert(largeKeys[1e100] == 3 and largeKeys[math.huge] == 4)
    local count = 0; for k,v in largeKeys do count += 1; assert(largeKeys[k] == v) end
    assert(count == 4 and largeKeys[0] == nil)
  )");
  CHECK(r.stats().modules == 1 && !r.stats().faulted);
  r.tick(1.25);
  r.evaluate("assert(CP.PTS == 1.25)");
  rejects([&] { r.tick(1); });
  CHECK(!r.stats().faulted);
  r.tick();
  r.evaluate("assert(CP.PTS == nil)");
}
void script_arc_coordinates(const Fixture& f) {
  script::Runtime r(f.asset, {"main", 100, 100});
  r.evaluate(R"(
    local root=CP.DPLayer.create();root.frame=CP.DPRect(0,0,100,100)
    root.backgroundColor=CP.DPColor(0,0,0,1)
    shape=CP.DPShapeLayer.create();shape.frame=CP.DPRect(0,0,100,100)
    local path=CP.DPPath.create()
    -- DP arcs measure angles with positive y upward; layer coordinates point
    -- downward. This dial starts at bottom-left and sweeps through the left.
    path:arcTo(CP.DPPoint(50,50),35,-3*math.pi/4,-math.pi/4,true)
    shape.path=path;shape.strokeColor=CP.DPColor(1,1,1,1)
    shape.lineWidth=6;shape.strokeEnd=1/3
    root:addSublayer(shape);CP.setRootLayer('main',root)
  )");
  auto image = r.render();
  CHECK(image.pixels[50 * 100 + 15] ==
        0xffffffff);  // First third of a 270-degree dial passes its leftmost point.
  CHECK(image.pixels[15 * 100 + 50] == 0xff000000);
  CHECK(image.pixels[85 * 100 + 50] == 0xff000000);
  r.evaluate(R"(
    local path=CP.DPPath.create()
    path:arcTo(CP.DPPoint(50,50),35,-3*math.pi/4,-math.pi/4,false)
    shape.path=path;shape.strokeEnd=1
  )");
  image = r.render();
  CHECK(image.pixels[85 * 100 + 50] ==
        0xffffffff);  // The opposite sweep is the short lower quarter.
  CHECK(image.pixels[50 * 100 + 15] == 0xff000000);
  CHECK(image.pixels[15 * 100 + 50] == 0xff000000);
}
void script_line_caps(const Fixture& f) {
  script::Runtime r(f.asset, {"main", 100, 100});
  r.evaluate(R"(
    local caps=CP.DPShapeLayer.LINECAP
    assert(caps.ROUND==0 and caps.SQUARE==1 and caps.BUTT==2)
    local root=CP.DPLayer.create();root.frame=CP.DPRect(0,0,100,100)
    shape=CP.DPShapeLayer.create();shape.frame=CP.DPRect(0,0,100,100)
    local path=CP.DPPath.create();path:moveTo(CP.DPPoint(30,50));path:lineTo(CP.DPPoint(70,50))
    shape.path=path;shape.strokeColor=CP.DPColor(1,1,1,1);shape.lineWidth=20
    assert(shape.lineCap==caps.BUTT)
    shape.lineCap=2;assert(shape.lineCap==caps.BUTT)
    root:addSublayer(shape);CP.setRootLayer('main',root)
  )");
  CHECK(r.render().pixels[50 * 100 + 25] == 0);  // Raw asset value 2 stops at the endpoint.
  r.evaluate("shape.lineCap=CP.DPShapeLayer.LINECAP.ROUND;assert(shape.lineCap==0)");
  auto rounded = r.render();
  CHECK(rounded.pixels[50 * 100 + 25] == 0xffffffff && rounded.pixels[41 * 100 + 21] == 0);
  r.evaluate("shape.lineCap=CP.DPShapeLayer.LINECAP.SQUARE;assert(shape.lineCap==1)");
  CHECK(r.render().pixels[41 * 100 + 21] == 0xffffffff);  // Named square keeps a real extension.
}
void plist_and_messages(const Fixture& f) {
  script::Runtime r(f.asset, {"main"});
  r.set_connected(true);
  r.evaluate(R"(
    local inner = CP.createPlist({type=17, data={name='test', value=-4, ok=true}})
    assert(#inner > 8)
    local outer = CP.createPlist({type=2,ssn=1,psn=2,asn=1,data=inner})
    local decoded = CP.parsePlist(outer)
    assert(type(decoded.data) == 'userdata')
    assert(CP.parsePlist(decoded.data).data.value == -4)
    assert(CP.sendMessage(outer))
    local empty = CP.parsePlist('<?xml version="1.0"?><plist version="1.0"><array/></plist>')
    assert(CP.sendMessage(CP.createPlist(empty)))
    local cyclic = {}; cyclic.child = cyclic
    assert(not pcall(CP.createPlist, cyclic))
    assert(not pcall(CP.createPlist, {1, a=2}))
    assert(not pcall(CP.createPlist, {[2]=1}))
    assert(not pcall(CP.createPlist, {value=0/0}))
    assert(not pcall(CP.createPlist, {value='a\000b'}))
    assert(not pcall(CP.createPlist, {value=string.char(0xc0,0xaf)}))
    assert(not pcall(CP.createPlist, {[string.char(0xff)]=1}))
    assert(CP.parsePlist(CP.createPlist({value='écran 🚗'})).value == 'écran 🚗')
    assert(not pcall(CP.parsePlist, 'not a plist'))
  )");
  auto output = r.take_outgoing();
  CHECK(output);
  const auto packet = decode(*output);
  CHECK(packet.type == MessageType::command && packet.sequence == 2 && !packet.data.empty());
  CHECK(r.receive(*output));
  r.evaluate(R"(
    local readable, writable, err = CP.status()
    assert(readable and writable and err == nil)
    assert(CP.parsePlist(CP.receiveMessage()).psn == 2)
    local value, error = CP.receiveMessage()
    assert(value == nil and error == 'timeout')
  )");
  auto empty = r.take_outgoing();
  CHECK(empty);
  CHECK(r.receive(*empty));
  r.evaluate(R"(
    local a = CP.parsePlist(CP.receiveMessage()); a[1] = 'x'
    assert(CP.parsePlist(CP.createPlist(a))[1] == 'x')
  )");
  CHECK(!r.take_outgoing());
  for (int i = 0; i < 64; ++i)
    CHECK(r.receive(*output));
  CHECK(!r.receive(*output));
  r.set_connected(false);
  CHECK(!r.receive(*output));
  r.set_connected(true);
  r.evaluate(R"(
    assert(not CP.status())
    for i=1,64 do assert(CP.sendMessage(CP.createPlist({value=i}))) end
    local success, err = CP.sendMessage(CP.createPlist({value=65}))
    assert(not success and err == 'timeout')
  )");
  for (int i = 0; i < 64; ++i)
    CHECK(r.take_outgoing());
  CHECK(!r.take_outgoing());
}
void remote_map_probe(const Fixture& f) {
  auto asset = f.asset;
  script::Runtime default_cluster(asset, {"Cluster_Display"});
  default_cluster.evaluate(R"(
    require('r/PackageData')
    local zone='zone-fixture'
    local layout='layout-fixture'
    local map='map-fixture'
    local before={type=5,data={layoutID=layout,activeComponents={}}}
    assert(next(CP.parsePlist(CP.createPlist(before)).data.activeComponents)==nil)
  )");
  script::Runtime cluster(asset, {"Cluster_Display"});
  cluster.select_remote_map_component();
  cluster.evaluate(R"(
    local zone='zone-fixture'
    local layout='layout-fixture'
    local map='map-fixture'
    require('r/PackageData')
    local request={type=5,data={layoutID=layout,activeComponents={}}}
    local sent=CP.parsePlist(CP.createPlist(request))
    assert(sent.data.activeComponents[zone]==map)
    local other={type=5,data={layoutID='other',activeComponents={}}}
    assert(next(CP.parsePlist(CP.createPlist(other)).data.activeComponents)==nil)
    local selected={type=5,data={layoutID=layout,activeComponents={[zone]='already-selected'}}}
    assert(CP.parsePlist(CP.createPlist(selected)).data.activeComponents[zone]=='already-selected')
    local ambiguous={type=5,data={layoutID='ambiguous-layout',activeComponents={}}}
    assert(next(CP.parsePlist(CP.createPlist(ambiguous)).data.activeComponents)==nil)
  )");
  script::Runtime main(asset, {"Center_Display"});
  main.evaluate(R"(
    require('r/PackageData')
    local request={type=5,data={layoutID='layout-fixture',activeComponents={}}}
    assert(next(CP.parsePlist(CP.createPlist(request)).data.activeComponents)==nil)
  )");
  rejects([&] { cluster.select_remote_map_component(); });
}
void tasks(const Fixture& f) {
  script::Runtime r(f.asset, {"main"});
  r.evaluate(R"(
    count = 0
    first = CP.DPLuaTask.create(); first.name = 'first'
    assert(first.name == 'first' and first:getClassName() == 'DPLuaTask')
    first:run(function() count += 1; coroutine.yield(); count += 10 end)
    assert(first:isBusy())
  )");
  r.tick();
  r.evaluate("assert(count == 1 and first:isBusy())");
  r.tick();
  r.evaluate("assert(count == 11 and not first:isBusy())");
  CHECK(r.stats().active_tasks == 0);
  r.evaluate(R"(
    first:run(function() first:stop(); coroutine.yield(); error('must never resume') end)
  )");
  r.tick();
  CHECK(r.stats().active_tasks == 0);
  r.tick();
  r.evaluate(R"(
    local second = CP.DPLuaTask.create()
    first:run(function()
      second:stop()
      second:run(function() count += 100 end)
    end)
    second:run(function() error('cancelled generation must not run') end)
  )");
  r.tick();
  r.evaluate("assert(count == 11)");
  r.tick();
  r.evaluate("assert(count == 111)");
  CHECK(r.stats().active_tasks == 0);
  r.evaluate("first:run(function() count += 1000 end); first:stop()");
  r.tick();
  r.evaluate("assert(count == 111)");
}
void resource_limits(const Fixture& f) {
  script::Limits limits;
  limits.startup_ms = 30;
  limits.frame_ms = 10;
  script::Runtime cpu(f.asset, {"main"}, limits);
  auto start = std::chrono::steady_clock::now();
  rejects([&] { cpu.evaluate("while true do pcall(function() while true do end end) end"); });
  CHECK(cpu.stats().faulted);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
  rejects([&] { cpu.evaluate("return"); });
  script::Runtime task_cpu(f.asset, {"main"}, limits);
  task_cpu.evaluate("CP.DPLuaTask.create():run(function() while true do end end)");
  rejects([&] { task_cpu.tick(); });
  CHECK(task_cpu.stats().faulted);
  limits.memory_bytes = 1024 * 1024;
  limits.startup_ms = 1000;
  script::Runtime memory(f.asset, {"main"}, limits);
  rejects(
      [&] { memory.evaluate("local x={}; for i=1,100000 do x[i]=string.rep('x',1024)..i end"); });
  CHECK(memory.stats().faulted && memory.stats().memory_bytes <= limits.memory_bytes);
  limits.tasks = 2;
  script::Runtime count(f.asset, {"main"}, limits);
  count.evaluate(R"(
    a=CP.DPLuaTask.create(); b=CP.DPLuaTask.create()
    assert(not pcall(CP.DPLuaTask.create))
  )");
  CHECK(!count.stats().faulted);
  script::Runtime fault(f.asset, {"main"});
  fault.set_connected(true);
  rejects(
      [&] { fault.evaluate("assert(CP.sendMessage(CP.createPlist({value=1}))); error('fault')"); });
  CHECK(fault.stats().faulted && !fault.take_outgoing());
}
void budget_fault_location(Fixture& f) {
  f.write("Test/Hot.lua", "return function()\nwhile true do end\nend");
  f.write("Test/Wrapper.lua", "return function(f)\nreturn pcall(f)\nend");
  script::Limits limits;
  limits.frame_ms = 5;
  script::Runtime runtime(f.asset, {"main"}, limits);
  runtime.evaluate(
      "local hot=require('Test.Hot'); local wrapper=require('Test.Wrapper'); "
      "CP.DPLuaTask.create():run(function() wrapper(hot) end)");
  std::string failure;
  try {
    runtime.tick();
  } catch (const std::runtime_error& error) {
    failure = error.what();
  }
  CHECK(runtime.stats().faulted);
  CHECK(failure.find("budgetMs=5") != failure.npos);
  CHECK(failure.find("budgetStack=Test/Hot.lua:2") != failure.npos);
  CHECK(failure.find("while true") == failure.npos);
  rejects([&] { runtime.tick(); });
}
void scheduler_pause_budget(const Fixture& f) {
  script::Limits limits;
  limits.startup_ms = 100;
  limits.frame_ms = 5;
  script::Runtime paused(f.asset, {"main"}, limits);
  paused.bind_log_sink([](int, std::string_view, std::string_view) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  });
  paused.evaluate("CP.DPLuaTask.create():run(function() CP.log(3,'test','pause') end)");
  paused.tick();
  CHECK(!paused.stats().faulted);
  script::Runtime blocked(f.asset, {"main"}, limits);
  blocked.bind_log_sink([](int, std::string_view, std::string_view) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  });
  blocked.evaluate("CP.DPLuaTask.create():run(function() CP.log(3,'test','blocked') end)");
  rejects([&] { blocked.tick(); });
  CHECK(blocked.stats().faulted);
}
void native_callback_budget(const Fixture& f) {
  script::Limits limits;
  limits.startup_ms = 100;
  limits.frame_ms = 5;
  script::Runtime runtime(f.asset, {"main"}, limits);
  runtime.bind_log_sink([](int, std::string_view, std::string_view) {
    const auto started = thread_cpu_ns();
    while (thread_cpu_ns() - started < 10000000ULL) {
    }
  });
  runtime.evaluate("CP.DPLuaTask.create():run(function() CP.log(3,'test','native CPU') end)");
  rejects([&] { runtime.tick(); });
  CHECK(runtime.stats().faulted);
  rejects([&] { runtime.tick(); });
}
void display_channel(const Fixture& f) {
  rejects([&] { script::Channel bad(f.asset, {}); });
  rejects([&] { script::Channel bad(f.asset, {{"main"}, {"main"}}); });
  script::Channel channel(f.asset, {{"main"}, {"dash"}});
  channel.set_connected(true);
  CHECK(!channel.take_outgoing());  // Asset session, not this adapter, initiates RESET.
  const auto data = encode(Packet{});
  CHECK(channel.receive(ui::encode({"MAIN", data})) == script::Delivery::unknown_display);
  CHECK(channel.receive(Bytes{0xff}) == script::Delivery::malformed);
  for (const auto* id : {"main", "dash"}) {
    channel.runtime(id).evaluate(R"(
      local name = CP.enumerateOutputStreams()[1].identifier
      for i=1,3 do assert(CP.sendMessage(CP.createPlist({display=name,number=i}))) end
    )");
  }
  for (int i = 0; i < 6; ++i) {
    const auto bytes = channel.take_outgoing();
    CHECK(bytes);
    const auto message = ui::decode(*bytes);
    CHECK(message.display_uuid == (i % 2 == 0 ? "main" : "dash"));
    CHECK(channel.receive(*bytes) == script::Delivery::accepted);
  }
  CHECK(!channel.take_outgoing());
  for (const auto* id : {"main", "dash"}) {
    channel.runtime(id).evaluate(R"(
      for i=1,3 do
        local message = CP.parsePlist(CP.receiveMessage())
        assert(message.display == CP.enumerateOutputStreams()[1].identifier and message.number == i)
      end
      assert(not CP.status())
    )");
  }
  for (int i = 0; i < 64; ++i)
    CHECK(channel.receive(ui::encode({"main", data})) == script::Delivery::accepted);
  CHECK(channel.receive(ui::encode({"main", data})) == script::Delivery::backpressure);
  CHECK(channel.receive(ui::encode({"dash", data})) == script::Delivery::accepted);
  rejects([&] { channel.runtime("main").evaluate("error('fault this display')"); });
  CHECK(channel.receive(ui::encode({"main", data})) == script::Delivery::faulted);
  CHECK(!channel.runtime("dash").stats().faulted);
  channel.set_connected(false);
  CHECK(!channel.take_outgoing());
  CHECK(channel.receive(ui::encode({"dash", data})) == script::Delivery::backpressure);
}
void verification_reports(const Fixture& f) {
  script::Runtime unbound(f.asset, {"unbound"});
  unbound.evaluate("assert(not pcall(CP.UpdateVerifiedUIElements,{}))");
  script::Runtime r(f.asset, {"cluster"}), other(f.asset, {"main"});
  std::vector<script::VerificationReport> reports;
  unsigned other_count = 0;
  bool accept = true;
  r.bind_verification_reporter([&](const auto& report) {
    if (!accept) throw std::runtime_error("monitor unavailable");
    reports.push_back(report);
  });
  other.bind_verification_reporter([&](const auto& report) {
    CHECK(report.display_identifier == "main");
    ++other_count;
  });
  r.evaluate(R"(
    region={originX=-10,originY=20,width=120,height=30,identifier='speed',presentationStyle='default-new'}
    assert(select('#',CP.UpdateVerifiedUIElements({region}))==0)
    region.width=1
  )");
  CHECK(reports.size() == 1 && reports[0].display_identifier == "cluster" && !reports[0].pts);
  CHECK((reports[0].regions ==
         std::vector<script::VerificationRegion>{{-10, 20, 120, 30, "speed", "default-new"}}));
  r.tick(4.25);
  r.evaluate("CP.UpdateVerifiedUIElements({})");
  CHECK(reports.size() == 2 && reports.back().regions.empty() && reports.back().pts == 4.25);
  r.evaluate(R"(
    for _,input in ipairs({{[2]=region},{region,extra=1},{false}}) do
      assert(not pcall(CP.UpdateVerifiedUIElements,input))
    end
    local huge={};for i=1,129 do huge[i]=region end
    assert(not pcall(CP.UpdateVerifiedUIElements,huge))
    for key,values in pairs({width={-1,32769,0/0,'2',1.2},originX={-32769,math.huge},
        identifier={'',string.rep('a',257),'bad\0id','\255'},presentationStyle={false}}) do
      for _,value in ipairs(values) do
        local entry=table.clone(region);entry[key]=value
        assert(not pcall(CP.UpdateVerifiedUIElements,{entry}))
      end
    end
    local missing=table.clone(region);missing.width=nil
    setmetatable(missing,{__index=function() error('metamethod must not run') end})
    assert(not pcall(CP.UpdateVerifiedUIElements,{missing}))
    local extra=table.clone(region);extra.verified=true
    assert(not pcall(CP.UpdateVerifiedUIElements,{extra}))
  )");
  CHECK(reports.size() == 2);
  accept = false;
  r.evaluate("assert(not pcall(CP.UpdateVerifiedUIElements,{region}))");
  CHECK(reports.size() == 2);
  other.evaluate("CP.UpdateVerifiedUIElements({})");
  CHECK(other_count == 1 && reports.size() == 2);
  rejects([&] { r.bind_verification_reporter([](const auto&) {}); });
}
void foreground_lifecycle(const Fixture& f) {
  script::Runtime r(f.asset, {"cluster"});
  r.evaluate("assert(not CP.isForegrounded and CP.PTS==nil)");
  r.set_foregrounded(true);
  r.evaluate("assert(CP.isForegrounded and CP.PTS==nil)");
  r.set_connected(true);
  r.set_connected(false);
  r.evaluate("assert(CP.isForegrounded)");
  r.tick(4.5);
  r.set_foregrounded(false);
  r.evaluate("assert(not CP.isForegrounded and CP.PTS==4.5)");
  r.evaluate("assert(not pcall(function() CP.isForegrounded=true end))");
  rejects([&] { r.evaluate("error('fault')"); });
  rejects([&] { r.set_foregrounded(true); });
}
void diagnostic_sink(const Fixture& f) {
  script::Runtime r(f.asset, {"main"});
  std::size_t received = 0;
  r.evaluate("CP.log(4,'test','default private log')");
  r.bind_log_sink([&](int level, std::string_view tag, std::string_view message) {
    CHECK(level == 3 && tag == "test" && message == "bounded record");
    ++received;
  });
  r.evaluate("CP.log(3,'test','bounded record')");
  CHECK(received == 1);
  r.bind_log_sink({});
  r.evaluate("CP.log(4,'test','disabled log')");
  CHECK(received == 1);
  CHECK(r.stats().log_records == 3);
}
void session_diagnostics(const Fixture& f) {
  using Kind = script::SessionDiagnosticKind;
  script::Runtime r(f.asset, {"Cluster_Display"});
  std::vector<script::SessionDiagnostic> events;
  r.bind_session_diagnostic_sink([&](const auto& event) { events.push_back(event); });
  std::size_t raw_records = 0;
  r.bind_log_sink([&](int level, std::string_view, std::string_view) {
    CHECK(level == 2);
    ++raw_records;
  });
  r.set_connected(true);
  r.set_foregrounded(true);
  r.evaluate(R"(
    assert(CP.sendMessage(CP.createPlist({retained=true})))
    local messages={
      {'ClusterControlSession','Starting ...'},
      {'ClusterControlSession','Resetting...'},
      {'ClusterControlSession','Received reset from peer {id="private-peer",secret="payload"}'},
      {'ClusterControlSession','Received reset complete from peer {id="private-peer",secret="payload"}'},
      {'ClusterControlSession','Ready to send/receive data.'},
      {'ClusterControlSession','Reset Complete Watchdog timed out!'},
      {'ClusterControlSession','Still waiting for reset complete, forcing reset ...'},
      {'ClusterControlSession','Received unexpected reset complete message id private-peer, expected private-other'},
      {'ClusterControlSession','Received unexpected reset complete received while in state 999 ... ignoring'},
      {'ClusterControlSession','Invalid ssn. Received 999, Expected 1000'},
      {'ClusterControlSession','Invalid old psn. Received 999, Expected 1000'},
      {'ClusterControlSession','Detected missing message psn. Received 999, Expected 1000'},
      {'ClusterControlSession','Invalid ack sn. Received 999, Expected in range [1...2)'},
      {'ClusterControlSession','unable to parse incoming data'},
      {'ClusterControlSession','Failed to send message: private-transport-error'},
      {'LayoutController','Queuing requestLayout command: {layoutID="private-layout",data="secret"}'},
      {'LayoutController','Not ready to send requestLayout command, _state: private-state'},
      {'Workspace','Layout change started, preparing layout private-layout'},
      {'Workspace','Layout change ended, showing remote UI for layout private-layout'},
      {'Workspace','Transition watchdog timer fired'},
      {'Metadata','Invalid transition metadata: {private="data"}'},
      {'LayoutController','Showing remote UI (animated = true, delay = private-delay)'},
      {'LayoutController','Hiding remote UI (animated = true, disconnected = private-data)'},
      {'LayoutController',"Couldn't get current video stream PTS to check freshness"},
      {'Workspace','Video stream freshness is late.'}
    }
    for _,message in ipairs(messages) do
      CP.log(2,'private-display | '..message[1],message[2])
    end
    -- Similar text from an unrelated module, arbitrary dictionaries, and
    -- signposts must never enter the safe session sink.
    CP.log(2,'Other','Resetting...')
    CP.log(2,'ClusterControlSession','Sending message: {private="payload"}')
    CP.log(2,'Workspace','arbitrary private log')
    CP.signpostEmitEvent('private-display | private-signpost')
  )");
  CHECK(events.size() == 25 && raw_records == 29 && r.stats().log_records == 28);
  CHECK(r.take_outgoing() &&
        !r.take_outgoing());  // Observation cannot reset the real message queues.
  std::uint64_t previous_time = 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& event = events[i];
    CHECK(std::size_t(event.kind) == i && event.count == 1 && event.connected &&
          event.foregrounded);
    CHECK(event.monotonic_ns >= previous_time && event.monotonic_ns > 0);
    previous_time = event.monotonic_ns;
    const auto name = script::session_diagnostic_name(event.kind);
    CHECK(!name.empty() && name.size() <= 48 && name.find("private") == name.npos &&
          name.find("payload") == name.npos);
  }
  CHECK(events[std::size_t(Kind::transition_watchdog)].kind == Kind::transition_watchdog);
  CHECK(script::session_diagnostic_name(static_cast<Kind>(255)) == "unknown");
  rejects([&] { r.bind_session_diagnostic_sink([](const auto&) {}); });
  CHECK(!r.stats().faulted);

  script::Runtime bounded(f.asset, {"Cluster_Display"});
  events.clear();
  bounded.bind_session_diagnostic_sink([&](const auto& event) { events.push_back(event); }, 10);
  bounded.evaluate(R"(
    for i=1,100000 do CP.log(2,'ClusterControlSession','Resetting...') end
    CP.log(2,'Workspace','Transition watchdog timer fired')
  )");
  CHECK(events.size() == 10 && events.back().kind == Kind::session_reset &&
        events.back().count == 256);
  for (const auto& event : events)
    CHECK(!event.connected && !event.foregrounded);

  script::Runtime per_kind(f.asset, {"Cluster_Display"});
  events.clear();
  per_kind.bind_session_diagnostic_sink([&](const auto& event) { events.push_back(event); }, 256);
  per_kind.evaluate(R"(
    for i=1,100000 do CP.log(2,'ClusterControlSession','Resetting...') end
    CP.log(2,'Workspace','Transition watchdog timer fired')
  )");
  CHECK(events.size() == 17 && events[15].count == 16384 &&
        events.back().kind == Kind::transition_watchdog);
  script::Runtime invalid(f.asset, {"Cluster_Display"});
  rejects([&] { invalid.bind_session_diagnostic_sink([](const auto&) {}, 0); });
  rejects([&] { invalid.bind_session_diagnostic_sink([](const auto&) {}, 257); });
  rejects([&] { invalid.bind_session_diagnostic_sink({}); });
  CHECK(!invalid.stats().faulted);
}
void optional_host_calls(const Fixture& f) {
  std::map<std::string, Bytes> saved;
  auto read = [&](std::string_view key) -> std::optional<Bytes> {
    const auto found = saved.find(std::string(key));
    return found == saved.end() ? std::nullopt : std::optional(found->second);
  };
  auto write = [&](std::string_view key, std::span<const std::uint8_t> bytes) {
    saved[std::string(key)] = Bytes(bytes.begin(), bytes.end());
  };
  script::Runtime first(f.asset, {"main"});
  first.evaluate("assert(not pcall(CP.loadPersistentStorage,'state'))");
  rejects([&] { first.bind_persistent_storage(read, write); });
  script::Runtime producer(f.asset, {"main"});
  producer.bind_persistent_storage(read, write);
  std::size_t events = 0;
  producer.bind_log_sink([&](int level, std::string_view tag, std::string_view message) {
    CHECK(level == 2 && tag == "signpost" && message == "render");
    ++events;
  });
  producer.evaluate(R"(
    assert(next(CP.loadPersistentStorage('state')) == nil)
    assert(CP.savePersistentStorage('state',{style='DigiBand',gauge={72,3200},enabled=true}))
    assert(not pcall(CP.savePersistentStorage,'../state',{}))
    assert(not pcall(CP.savePersistentStorage,'huge',{value=string.rep('x',300000)}))
    local repeated=string.rep('x',70000)
    assert(not pcall(CP.savePersistentStorage,'unreadable',{repeated,repeated,repeated,repeated}))
    local id=CP.signpostBeginInterval('cluster')
    assert(type(id)=='number')
    CP.signpostEmitEvent('render')
    CP.signpostEndInterval(id,'cluster')
    assert(not pcall(CP.signpostEndInterval,id,'cluster'))
    CP.readyToEnterForeground()
  )");
  CHECK(events == 1 && producer.stats().foreground_ready_requested && saved.size() == 1);
  script::Runtime consumer(f.asset, {"main"});
  consumer.bind_persistent_storage(read, write);
  consumer.evaluate(R"(
    local state=CP.loadPersistentStorage('state')
    assert(state.style=='DigiBand' and state.gauge[1]==72 and state.gauge[2]==3200 and state.enabled)
  )");
  CHECK(!consumer.stats().faulted);
  // A small encoded graph can alias the same large value repeatedly. Reject
  // that expansion before libplist allocates a tree, not after Lua conversion.
  const auto integer = [](Bytes& out, std::uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      out.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
  };
  const auto graph = [&](const std::vector<Bytes>& objects) {
    Bytes out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<std::size_t> offsets;
    for (const auto& object : objects) {
      offsets.push_back(out.size());
      out.insert(out.end(), object.begin(), object.end());
    }
    const auto table = out.size();
    for (const auto offset : offsets)
      integer(out, offset, 4);
    out.insert(out.end(), 6, 0);
    out.push_back(4);
    out.push_back(1);
    integer(out, objects.size(), 8);
    integer(out, 0, 8);
    integer(out, table, 8);
    return out;
  };
  Bytes blob{0x4f, 0x12};
  integer(blob, 128 * 1024, 4);
  blob.insert(blob.end(), 128 * 1024, 42);
  saved["amplified"] = graph({Bytes{0xa4, 1, 1, 1, 1}, blob});
  CHECK(saved["amplified"].size() < 256 * 1024);
  saved["cyclic"] = graph({Bytes{0xa1, 0}});
  saved["bad-reference"] = graph({Bytes{0xa1, 1}});
  script::Runtime bounded(f.asset, {"main"});
  bounded.bind_persistent_storage(read, write);
  bounded.evaluate(R"(
    assert(not pcall(CP.loadPersistentStorage,'amplified'))
    assert(not pcall(CP.loadPersistentStorage,'cyclic'))
    assert(not pcall(CP.loadPersistentStorage,'bad-reference'))
    assert(CP.loadPersistentStorage('state').style=='DigiBand')
  )");
  CHECK(!bounded.stats().faulted);
}
void startup_task_budget(const Fixture& f) {
  script::Limits limits;
  limits.startup_ms = 100;
  limits.frame_ms = 5;
  script::Runtime r(f.asset, {"main"}, limits);
  rejects([&] { r.start_tasks(); });
  r.tick(1);  // Presentation bootstrap before loading the entry point is valid.
  r.bind_log_sink([](int, std::string_view, std::string_view) {
    const auto started = thread_cpu_ns();
    while (thread_cpu_ns() - started < 12000000ULL) {
    }
  });
  r.evaluate(R"(
    CP.DPLuaTask.create():run(function()
      while true do
        CP.log(3,'test','startup task CPU')
        coroutine.yield()
      end
    end)
  )");
  r.start_tasks(1);
  CHECK(!r.stats().faulted);
  rejects([&] { r.start_tasks(1); });
  CHECK(!r.stats().faulted);
  // Initialization did not relax the subsequent normal deadline.
  rejects([&] { r.tick(1); });
  CHECK(r.stats().faulted);
  script::Runtime late(f.asset, {"main"}, limits);
  late.evaluate("return");
  late.tick();
  rejects([&] { late.start_tasks(); });
  limits.startup_ms = 30;
  script::Runtime infinite(f.asset, {"main"}, limits);
  infinite.evaluate("CP.DPLuaTask.create():run(function() while true do end end)");
  const auto start = std::chrono::steady_clock::now();
  rejects([&] { infinite.start_tasks(); });
  CHECK(infinite.stats().faulted);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
void shared_resource_cache() {
  int reads = 0;
  auto reader = script::cache_resources(
      [&](std::string_view path) {
        ++reads;
        if (path == "missing") throw std::runtime_error("missing immutable asset");
        return std::string("original");
      },
      9, 1);  // One one-character path and eight-byte value fill the cache.
  auto other_display = reader;
  auto first = reader("a");
  first[0] = 'X';
  CHECK(other_display("a") == "original" && reads == 1);
  CHECK(reader("b") == "original" && reader("b") == "original" && reads == 3);
  rejects([&] { reader("missing"); });
  rejects([&] { reader("missing"); });
  CHECK(reads == 5);
  CHECK(reader("a") == "original" && reads == 5);
  rejects([&] { script::cache_resources({}, 1, 1); });
  rejects([&] { script::cache_resources(reader, 65 * 1024 * 1024, 1); });
}
void verified_reader_boundary() {
  std::vector<std::string> reads;
  script::Runtime r(
      [&](std::string_view name) {
        reads.emplace_back(name);
        if (name == "Reader/Large.lua") return std::string(2200000, ' ') + "return 42";
        if (name == "over-limit") return std::string(4 * 1024 * 1024 + 1, 'x');
        if (name == "raw.txt") return std::string("fixture");
        throw std::runtime_error("missing resource");
      },
      {"main"});
  r.evaluate(R"(
    assert(require('Reader.Large')==42)
    assert(require('Reader.Large')==42)
    assert(CP.createAssetRAW('raw.txt')=='fixture')
  )");
  CHECK(reads.size() == 2);
  r.evaluate(R"(
    assert(not pcall(CP.createAssetRAW, '../outside'))
    assert(not pcall(CP.createAssetRAW, 'a//b'))
    assert(not pcall(CP.createAssetRAW, '/absolute'))
    assert(not pcall(CP.createAssetRAW, 'trailing/'))
  )");
  CHECK(reads.size() == 2);
  r.evaluate("assert(not pcall(CP.createAssetRAW, 'over-limit'))");
  CHECK(reads.size() == 3);
  rejects([] { script::Runtime invalid(script::ResourceReader{}, {"main"}); });
}
void startup_layout_selection(const Fixture& f) {
  const auto reader = [](std::string_view path) {
    if (path == "Config.json") return std::string("original configuration");
    throw std::runtime_error("missing resource");
  };
  script::Runtime default_layout(reader, {"Cluster_Display"});
  default_layout.evaluate("assert(CP.createAssetRAW('Config.json')=='original configuration')");
  script::Runtime selected(reader, {"Cluster_Display"});
  selected.select_startup_layout("00000000-1111-2222-3333-444444444444");
  selected.evaluate(R"(
    assert(CP.createAssetRAW('Config.json')=='{"STARTUP":{"LAYOUT":{"ID":"00000000-1111-2222-3333-444444444444"}}}')
  )");
  rejects([&] { selected.select_startup_layout("00000000-1111-2222-3333-444444444444"); });
  for (const auto* invalid : {"", "not-a-layout", "00000000-1111-2222-3333-44444444444Z",
                              "000000001111-2222-3333-4444444444444"}) {
    script::Runtime r(f.asset, {"Cluster_Display"});
    rejects([&] { r.select_startup_layout(invalid); });
  }
}
void stable_number_rounding(Fixture& f) {
  fs::create_directories(f.path / "APL");
  f.write("APL/StableNumberLayer.lua", R"(
    return {marker='original',new=function()
      local layer={_maximumFractionDigits=0}
      function layer:setNumber(value, skip) self.number=value;self.skip=skip;return self end
      return layer
    end}
  )");
  f.write("APL/LuaUtils.lua", R"(
    return {numberToString=function(value,digits)
      return string.format('%.'..tostring(digits or 0)..'f',value)
    end}
  )");
  script::Runtime r(f.asset, {"Cluster_Display"});
  r.evaluate(R"(
    local module=require 'APL.StableNumberLayer'
    assert(module.marker=='original' and module==require 'APL.StableNumberLayer')
    assert(not pcall(function() module.new=false end))
    local layer=module.new()
    for _, value in ipairs({19.6,20,20.4}) do
      assert(layer:setNumber(value,true)==layer and layer.number==20 and layer.skip)
    end
    layer:setNumber(99.6);assert(layer.number==100)
    layer._maximumFractionDigits=1
    layer:setNumber(19.6);assert(layer.number==19.6)
    layer:setNumber(nil);assert(layer.number==nil)
  )");
}
void remote_component_observation(Fixture& f) {
  f.write("Layout.lua", R"(
    local module={marker='original'}
    function module.new(info)
      local layout={localComponents={},calls=0}
      function layout:setActiveDCAComponentIDs(components)
        self.calls=self.calls+1
        if components.fail then error('setter failed') end
        return self
      end
      function layout:getActiveDCAComponentIDs() return table.clone(self.localComponents) end
      return layout
    end
    return module
  )");
  script::Runtime r(f.asset, {"Cluster_Display"});
  r.enable_live_layout_selection();
  r.evaluate(R"(
    local module=require('Layout')
    assert(module.marker=='original')
    local info={getZones=function() return {{id='zone',type={dca={}},components={
      {id='map',type='primary',slots={}},
      {id='local',type='primary',slots={{}}},
      {id='overlay',type='overlay',slots={}}
    }}} end}
    local layout=module.new(info)
    assert(next(layout:getActiveDCAComponentIDs())==nil)
    assert(layout:setActiveDCAComponentIDs({zone='map'})==layout)
    assert(layout.calls==1 and layout:getActiveDCAComponentIDs().zone=='map')
    assert(not pcall(function() layout:setActiveDCAComponentIDs({zone='unknown',fail=true}) end))
    assert(layout:getActiveDCAComponentIDs().zone=='map')
    layout.localComponents.zone='local'
    assert(layout:getActiveDCAComponentIDs().zone=='local')
    layout.localComponents.zone=nil
    for _, id in ipairs({'local','overlay','unknown'}) do
      layout:setActiveDCAComponentIDs({zone=id})
      assert(next(layout:getActiveDCAComponentIDs())==nil)
    end
    layout:setActiveDCAComponentIDs({zone='map'})
    layout:setActiveDCAComponentIDs({})
    assert(next(layout:getActiveDCAComponentIDs())==nil)
    assert(next(module.new(info):getActiveDCAComponentIDs())==nil)
  )");
}
void scene_observation(Fixture& f) {
  f.write("Workspace.lua",
          "return {STATE={LOGOS_TRANSITION='logos',STARTUP='startup',DRIVE='drive'}}");
  f.write("LayoutController.lua", R"(
    local state = {activeState='none'}
    local module = {marker='original'}
    function module.new() return {_workspace={getState=function() return state end}} end
    function module.phase(value) state.activeState=value end
    return module
  )");
  script::Runtime r(f.asset, {"Cluster_Display"});
  r.enable_scene_observation();
  CHECK(r.scene_phase() == script::Runtime::ScenePhase::unavailable);
  r.evaluate("require('LayoutController').new()");
  CHECK(r.scene_phase() == script::Runtime::ScenePhase::other);
  for (const auto& [name, phase] :
       std::array{std::pair{"logos", script::Runtime::ScenePhase::logos},
                  std::pair{"startup", script::Runtime::ScenePhase::startup},
                  std::pair{"drive", script::Runtime::ScenePhase::drive}}) {
    r.evaluate(std::string("require('LayoutController').phase('") + name + "')");
    CHECK(r.scene_phase() == phase);
  }
  rejects([&] { r.enable_scene_observation(); });
  r.evaluate("require('LayoutController').phase(nil)");
  rejects([&] { r.scene_phase(); });
  CHECK(r.stats().faulted);
  script::Runtime bad(f.asset, {"Cluster_Display"});
  bad.enable_scene_observation();
  bad.evaluate(
      "require('LayoutController').new()._workspace.getState=function() error('broken') end");
  rejects([&] { bad.scene_phase(); });
  CHECK(bad.stats().faulted);
  rejects([&] { bad.scene_phase(); });
}
void live_layout_selection(Fixture& f) {
  constexpr auto first = "00000000-1111-2222-3333-444444444444";
  constexpr auto second = "00000000-1111-2222-3333-555555555555";
  f.write("LayoutController.lua", R"(
    local ids={'00000000-1111-2222-3333-444444444444','00000000-1111-2222-3333-555555555555'}
    local current
    local module={marker='original module'}
    local state={appearance={targetAppearanceMode=7},busy=false,idle=true}
    function state:isTransitioning() return self.busy end
    function state:isIdle() return self.idle and not self.busy end
    local infos={}
    for i,id in ipairs(ids) do
      local info={id=id,name='Style '..i}
      function info:getID() return self.id end
      function info:getDisplayName() return self.name end
      info.palette='blue'
      function info:getPaletteID() return self.palette end
      local zone={id='00000000-1111-2222-3333-777777777777',selected='00000000-1111-2222-3333-888888888888'}
      local component={_name='Map'}
      function component:getID() return '00000000-1111-2222-3333-888888888888' end
      function zone:isDCA() return true end
      function zone:getID() return self.id end
      function zone:getComponentsByID() return {[component:getID()]=component} end
      function zone:getPrimaryComponentIDs() return {component:getID()} end
      function zone:getActiveComponent() return self.selected==component:getID() and component or nil end
      info._zones={[zone.id]=zone}
      function info:getZones()
        local instrument=self.normalized and {
          kind=self.normalized==true and 'inputStream' or self.normalized,
          streamIdentifier=self.input}
          or {kind={inputStream={identifier=self.input}}}
        local slots=self.input and {{instrument=instrument}} or {}
        return {{id=zone.id,type={dca={}},components={{id=component:getID(),type='primary',name='Map',slots=slots}}}}
      end
      function info:getActiveDCAComponentIDs() return {[zone.id]=zone.selected} end
      infos[i]=info
    end
    function module.new(argument)
      assert(argument==42)
      local c={active=infos[1],_layoutsById={},_theme={palettes={blue={displayName='Blue'},red={displayName='Red'}}}}
      for _,info in ipairs(infos) do c._layoutsById[info.id]=info end
      c._display={getLayouts=function(_,kind) assert(kind=='primary');return infos end}
      c._workspace={getState=function() return state end}
      c._session={getIsReady=function() return true end}
      function c._session:sendCommand(command)
        assert(command.type==5 and command.data.appearanceMode==7)
        assert(CP.sendMessage(CP.createPlist(command)))
      end
      function c:getActiveLayout() return self.active end
      function c:_sendLayoutRequest(target,appearance,fade_in,fade_out)
        assert(appearance==7 and fade_in and fade_out)
        assert(CP.sendMessage(CP.createPlist({requested=target.id})))
      end
      current=c
      return c,'constructor result'
    end
    function module.finish(id) current.active=current._layoutsById[id];state.busy=false end
    function module.appearance(palette, component)
      current.active.palette=palette
      for _, zone in pairs(current.active._zones) do zone.selected=component end
    end
    function module.video(visible, opacity, pts)
      local layer={getVisible=function() return visible end,
                   getOpacity=function() return opacity end}
      current._iPhoneVideoInputStream={getRootLayer=function() return layer end,
                                      getCurrentPTS=function() return pts end}
    end
    function module.busy(value) state.busy=value end
    function module.idle(value) state.idle=value end
    function module.requiredInput(identifier, normalized)
      current.active.input=identifier
      current.active.normalized=normalized
    end
    return module
  )");
  script::Runtime disabled(f.asset, {"Cluster_Display"});
  CHECK(disabled.layout_selection().choices.empty());
  script::Limits selection_limits;
  selection_limits.frame_ms = 5;
  // Initial catalogue polling must execute the prepared adapter rather than
  // compiling it on a control turn. Keep tight real CPU/wall checks afterward.
  script::Runtime r(f.asset, {"Cluster_Display"}, selection_limits);
  r.add_video_input({"fixture-assistance", 16, 16, 1.0 / 30, video::InputType::widget});
  r.enable_live_layout_selection();
  CHECK(r.layout_selection().choices.empty());
  r.set_connected(true);
  r.set_foregrounded(true);
  r.evaluate(R"(
    local module=require 'LayoutController'
    assert(module.marker=='original module' and module==require 'LayoutController')
    local c,result=module.new(42)
    assert(result=='constructor result' and c:getActiveLayout():getDisplayName()=='Style 1')
    assert(not pcall(function() module.new=false end))
  )");
  auto state = r.layout_selection();
  CHECK(state.ready && state.idle && !state.transitioning && state.active == first &&
        state.choices.size() == 2);
  // A connected local/default layout is not a completed phone transition.
  r.evaluate("require('LayoutController').idle(false)");
  CHECK(!r.layout_selection().idle &&
        r.layout_selection(first).result == script::LayoutRequest::busy && !r.take_outgoing());
  r.evaluate("require('LayoutController').idle(true)");
  CHECK(r.layout_selection(first).result == script::LayoutRequest::selected && !r.take_outgoing());
  state = r.layout_selection(second);
  CHECK(state.result == script::LayoutRequest::requested && state.active == first);
  CHECK(r.take_outgoing() &&
        !r.take_outgoing());  // Request never switches locally ahead of the phone.
  r.evaluate("require('LayoutController').busy(true)");
  CHECK(r.layout_selection(second).result == script::LayoutRequest::busy && !r.take_outgoing());
  r.evaluate("require('LayoutController').finish('00000000-1111-2222-3333-555555555555')");
  CHECK(r.layout_selection(second).result == script::LayoutRequest::selected);
  CHECK(r.layout_selection("00000000-1111-2222-3333-666666666666").result ==
        script::LayoutRequest::unknown);
  constexpr auto zone = "00000000-1111-2222-3333-777777777777";
  constexpr auto component = "00000000-1111-2222-3333-888888888888";
  state = r.cluster_selection();
  CHECK(state.palettes.size() == 2 && state.contents.size() == 1 && state.active_palette == "blue");
  CHECK(!state.phone_video);
  r.evaluate("require('LayoutController').video(true,0,nil)");
  state = r.cluster_selection();
  CHECK(state.phone_video && state.phone_video->visible && state.phone_video->opacity == 0 &&
        !state.phone_video->has_current_pts);
  r.evaluate("require('LayoutController').video(false,0.5,42)");
  state = r.cluster_selection();
  CHECK(state.phone_video && !state.phone_video->visible && state.phone_video->opacity == 0.5 &&
        state.phone_video->has_current_pts);
  script::ClusterSelectionRequest appearance{second, "red", zone, component};
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::requested);
  CHECK(r.take_outgoing().has_value());
  CHECK(r.cluster_selection().active_palette == "blue");  // Request is not confirmation.
  r.evaluate(
      "require('LayoutController').appearance('red','00000000-1111-2222-3333-888888888888')");
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::selected &&
        !r.take_outgoing());
  r.evaluate("require('LayoutController').requiredInput('missing-input')");
  CHECK(!r.cluster_selection().contents[0].available);
  CHECK(
      r.cluster_selection(appearance).result == script::LayoutRequest::unknown &&
      !r.take_outgoing());  // Even an observed active choice cannot bypass missing input admission.
  r.evaluate("require('LayoutController').requiredInput('missing-input',true)");
  CHECK(!r.cluster_selection().contents[0].available);
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::unknown &&
        !r.take_outgoing());
  r.evaluate("require('LayoutController').requiredInput('fixture-assistance',true)");
  CHECK(r.cluster_selection().contents[0].available);
  r.evaluate("require('LayoutController').requiredInput('missing-input','punchThrough')");
  CHECK(!r.cluster_selection().contents[0].available);
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::unknown &&
        !r.take_outgoing());
  r.evaluate("require('LayoutController').requiredInput('fixture-assistance','punchThrough')");
  CHECK(r.cluster_selection().contents[0].available);
  r.evaluate("require('LayoutController').requiredInput('fixture-assistance')");
  CHECK(r.cluster_selection().contents[0].available);
  r.evaluate("require('LayoutController').appearance('blue',nil)");
  CHECK(!r.cluster_selection().contents[0].active);
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::requested &&
        r.take_outgoing());
  CHECK(!r.cluster_selection().contents[0].active);  // Registration/request is not confirmation.
  r.evaluate("require('LayoutController').requiredInput(nil)");
  appearance.palette = "unknown";
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::unknown &&
        !r.take_outgoing());
  appearance.palette = "red";
  appearance.zone = first;
  CHECK(r.cluster_selection(appearance).result == script::LayoutRequest::unknown &&
        !r.take_outgoing());
  appearance.zone.clear();
  rejects([&] { r.cluster_selection(appearance); });
  r.set_connected(false);
  CHECK(r.layout_selection(first).result == script::LayoutRequest::unavailable &&
        !r.take_outgoing());
  rejects([&] { r.layout_selection("bad'); arbitrary_code()"); });
  CHECK(!r.stats().faulted);
  rejects([&] { r.enable_live_layout_selection(); });
}
int main() {
  try {
    shared_resource_cache();
    verified_reader_boundary();
    Fixture f;
    startup_layout_selection(f);
    modules_and_sandbox(f);
    script_arc_coordinates(f);
    script_line_caps(f);
    plist_and_messages(f);
    remote_map_probe(f);
    tasks(f);
    resource_limits(f);
    budget_fault_location(f);
    scheduler_pause_budget(f);
    native_callback_budget(f);
    display_channel(f);
    verification_reports(f);
    foreground_lifecycle(f);
    diagnostic_sink(f);
    session_diagnostics(f);
    optional_host_calls(f);
    startup_task_budget(f);
    stable_number_rounding(f);
    remote_component_observation(f);
    scene_observation(f);
    live_layout_selection(f);
    std::cout << "script runtime: sandbox, plist, transport, tasks and resource limits passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
