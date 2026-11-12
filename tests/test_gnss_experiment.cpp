#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <vector>
#include "experiment/gnss_experiment.hpp"

using Json = nlohmann::json;
namespace {
const std::string id="DCDWCNS1GNSSTEST0001";
const std::string capture="00000000-0000-4000-8000-000000000004";
const auto wall=std::chrono::system_clock::time_point{std::chrono::seconds{1800000000}};
const auto now=std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
std::filesystem::path Directory() {
  auto p=std::filesystem::temp_directory_path() / ("cns-gnss-test-"+std::to_string(getpid()));
  std::filesystem::remove_all(p); return p;
}
Json Request(std::string operation="gnss_capture_start", int action=3) {
  char action_id[37]{};
  std::snprintf(action_id, sizeof(action_id), "00000000-0000-4000-8000-%012x", action);
  Json j{{"schema_version",1},{"command_id","00000000-0000-4000-8000-000000000001"},
    {"request_id",action_id},
    {"session_id","00000000-0000-4000-8000-000000000002"},
    {"action_id",action_id},
    {"target",{{"device_id",id}}},{"lease_version",1},{"operation",operation},
    {"expires_at","2099-01-01T00:00:00.000Z"}};
  if(operation!="gnss_status") j["parameters"]={{"capture_id",capture}};
  if(operation=="gnss_capture_start") j["parameters"]["duration_seconds"]=1;
  return j;
}
Json Act(experiment::GnssExperiment& e, Json j, bool live=true) {
  auto o=e.Handle("gnsstest/"+id+"/experiment/set",j.dump(),id,live,wall,now);
  REQUIRE(o.publication); return Json::parse(o.publication->payload);
}

void Feed(experiment::GnssExperiment& e, const std::vector<std::uint8_t>& wire,
          std::uint16_t crc, std::uint32_t frame_id, std::uint8_t protocol=1,
          std::uint8_t flags=3, std::chrono::steady_clock::time_point received=now) {
  const auto count=(wire.size()+39)/40;
  for(std::size_t index=0;index<count;++index) {
    std::array<std::uint8_t,128> p{};
    p[0]=1;p[1]=protocol;p[2]=flags;p[3]=24;
    for(unsigned i=0;i<4;++i) {
      p[4+i]=static_cast<std::uint8_t>(frame_id>>(8*i));
      p[8+i]=static_cast<std::uint8_t>(0x12345678U>>(8*i));
    }
    p[12]=static_cast<std::uint8_t>(wire.size());
    p[13]=static_cast<std::uint8_t>(wire.size()>>8);
    p[14]=static_cast<std::uint8_t>(40*index);
    p[15]=static_cast<std::uint8_t>((40*index)>>8);
    p[16]=static_cast<std::uint8_t>(index);p[17]=static_cast<std::uint8_t>(count);
    p[18]=static_cast<std::uint8_t>(std::min<std::size_t>(40,wire.size()-40*index));
    p[19]=protocol==1?1:0;p[20]=protocol==2?1:0;p[21]=protocol==2?7:0;
    p[22]=static_cast<std::uint8_t>(crc);p[23]=static_cast<std::uint8_t>(crc>>8);
    std::copy_n(wire.begin()+40*index,p[18],p.begin()+24);
    mavlink_message_t message{};
    mavlink_msg_tunnel_pack(42,193,&message,0,0,0x8020,
        static_cast<std::uint8_t>(24+p[18]),p.data());
    e.OnMessage(message,wall,received);
  }
}

Json LongStart(int action=3) {
  auto request=Request("gnss_capture_start",action);
  request["parameters"]["duration_seconds"]=1800;
  return request;
}

std::vector<Json> Archive(const std::filesystem::path& path) {
  std::ifstream in(path);std::string line;std::vector<Json> result;
  while(std::getline(in,line)) result.push_back(Json::parse(line));
  return result;
}
}
TEST_CASE("GNSS capture is bounded by elapsed duration with a real archive") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  CHECK(e.Status()["capture_id"].is_null());CHECK(e.Status()["session_id"].is_null());
  auto ack=Act(e,Request()); CHECK(ack["status"]=="completed"); CHECK(e.Busy());
  CHECK(e.Status()["capture_id"]==capture);
  CHECK(e.Status()["session_id"]==Request()["session_id"]);
  auto repeat=Request(); repeat["command_id"]="00000000-0000-4000-8000-000000000009";
  CHECK(Act(e,repeat)["command_id"]==repeat["command_id"]);
  e.Tick(wall+std::chrono::seconds{2},now+std::chrono::seconds{2},true);
  CHECK_FALSE(e.Busy()); CHECK(e.Status()["end_reason"]=="duration_elapsed");
  CHECK(e.Status()["frames"]==0);
  std::ifstream log(directory/"current.jsonl"); std::string line; int lines=0;
  while(std::getline(log,line)) { CHECK(Json::parse(line).contains("kind")); ++lines; }
  CHECK(lines==2);
  CHECK(Act(e,Request("gnss_capture_start",5))["error_code"]=="capture_conflict");
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS batch wait changes during capture without dropping raw records") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  const std::vector<std::uint8_t> wire{'1','2','3','4','5','6','7','8','9'};
  e.SetBatchInterval(std::chrono::milliseconds{1000});
  Feed(e,wire,0x29B1,1);
  e.Tick(wall,now+std::chrono::milliseconds{999},true);
  CHECK(e.FrontBatch()==nullptr);
  e.SetBatchInterval(std::chrono::milliseconds{500});
  e.Tick(wall,now+std::chrono::milliseconds{999},true);
  REQUIRE(e.FrontBatch());
  CHECK(Json::parse(e.FrontBatch()->payload)["frames"].size()==1);
  e.ConfirmBatch();
  e.SetBatchInterval(std::chrono::milliseconds{60000});
  for(unsigned i=2;i<=33;++i) Feed(e,wire,0x29B1,i,1,3,now+std::chrono::seconds{1});
  REQUIRE(e.FrontBatch());
  CHECK(Json::parse(e.FrontBatch()->payload)["frames"].size()==32);
  e.ConfirmBatch();
  CHECK(e.Busy());
  Feed(e,wire,0x29B1,34,1,3,now+std::chrono::seconds{1});
  CHECK(Act(e,Request("gnss_capture_stop",5))["status"]=="completed");
  REQUIRE(e.FrontBatch());
  CHECK(Json::parse(e.FrontBatch()->payload)["frames"][0]["record_seq"]==34);
  CHECK(e.Status()["frames"]==34);
  CHECK(Archive(directory/"current.jsonl").size()==36);
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS retains binary zeros CRLF and BAD source checksums in archive and batches") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  const std::vector<std::uint8_t> nmea{36,71,80,71,71,65,44,49,42,48,48,13,10};
  const std::vector<std::uint8_t> ubx{0xB5,0x62,1,7,0,0,0,0};
  Feed(e,nmea,0xD34A,1);
  CHECK(e.Status()["transport_counters"]["fragments_received"]==0);
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  Feed(e,nmea,0xD34A,1,1,2);Feed(e,ubx,0x3350,2,2);
  e.Tick(wall+std::chrono::milliseconds{199},now+std::chrono::milliseconds{199},true);
  CHECK(e.FrontBatch()==nullptr);
  e.Tick(wall+std::chrono::milliseconds{200},now+std::chrono::milliseconds{200},true);
  const auto* publication=e.FrontBatch();REQUIRE(publication);
  CHECK(publication->topic=="gnsstest/"+id+"/gnss/raw/v1");
  CHECK(publication->qos==1);CHECK_FALSE(publication->retain);
  const auto batch=Json::parse(publication->payload);
  CHECK(batch["schema_version"]==1);CHECK(batch["batch_seq"]==1);
  CHECK(batch["device_id"]==id);CHECK(batch["capture_id"]==capture);
  REQUIRE(batch["frames"].size()==2);
  const auto& a=batch["frames"][0];const auto& b=batch["frames"][1];
  CHECK(a["raw_base64"]=="JEdQR0dBLDEqMDANCg==");
  CHECK(b["raw_base64"]=="tWIBBwAAAAA=");
  CHECK(a["raw_length"]==nmea.size());CHECK(b["raw_length"]==ubx.size());
  CHECK(a["source_checksum_valid"]==false);CHECK(b["source_checksum_valid"]==true);
  CHECK(a["record_seq"]==1);CHECK(b["record_seq"]==2);CHECK(a["elapsed_ms"]==0);
  CHECK(a["source"]["system_id"]==42);CHECK(a["source"]["component_id"]==193);
  CHECK(a["source"]["received_uptime_ms"]==0x12345678);CHECK(a["wire_crc16"]==0xD34A);
  CHECK(a["protocol"]=="nmea");CHECK(a["nmea_type"]==1);
  CHECK(b["protocol"]=="ubx");CHECK(b["ubx_class"]==1);CHECK(b["ubx_id"]==7);
  CHECK(e.Status()["frames"]==2);CHECK(e.Status()["raw_bytes"]==21);
  CHECK(e.Status()["source_checksum_bad_frames"]==1);
  e.ConfirmBatch();CHECK(e.Status()["confirmed_batches"]==1);CHECK(e.FrontBatch()==nullptr);
  CHECK(Act(e,Request("gnss_capture_stop",5),false)["status"]=="completed");
  const auto log=Archive(directory/"current.jsonl");REQUIRE(log.size()==4);
  CHECK(log.front()["kind"]=="metadata");CHECK(log.back()["kind"]=="summary");
  CHECK(log[1]["raw_base64"]==a["raw_base64"]);CHECK(log[2]["raw_base64"]==b["raw_base64"]);
  CHECK(log.back()["gnss"]["end_reason"]=="capture_stopped");
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS flushes at 32 frames and never exceeds the 16KiB JSON limit") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  const std::vector<std::uint8_t> small{'1','2','3','4','5','6','7','8','9'};
  for(unsigned i=1;i<=31;++i) Feed(e,small,0x29B1,i);
  CHECK(e.FrontBatch()==nullptr);Feed(e,small,0x29B1,32);
  REQUIRE(e.FrontBatch());CHECK(Json::parse(e.FrontBatch()->payload)["frames"].size()==32);
  e.ConfirmBatch();
  std::vector<std::uint8_t> big(1024);
  for(std::size_t i=0;i<big.size();++i) big[i]=static_cast<std::uint8_t>(i);
  for(unsigned i=33;i<=52;++i) Feed(e,big,0x758F,i,2);
  e.Tick(wall+std::chrono::milliseconds{200},now+std::chrono::milliseconds{200},true);
  unsigned count=0;std::uint64_t sequence=33,batch_sequence=2;
  while(const auto* publication=e.FrontBatch()) {
    CHECK(publication->payload.size()<=16384);
    const auto batch=Json::parse(publication->payload);
    CHECK(batch["batch_seq"]==batch_sequence++);CHECK(batch["frames"].size()<=32);
    for(const auto& frame:batch["frames"]) { CHECK(frame["record_seq"]==sequence++);++count; }
    e.ConfirmBatch();
  }
  CHECK(count==20);CHECK(e.Busy());
  e.TransportLost("mqtt_disconnected",wall);
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS bounded batch backpressure ends capture and blocks archive rotation until drained") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  const std::vector<std::uint8_t> wire{'1','2','3','4','5','6','7','8','9'};
  for(unsigned i=1;i<=288;++i) Feed(e,wire,0x29B1,i);
  CHECK_FALSE(e.Busy());CHECK(e.Status()["state"]=="failed");
  CHECK(e.Status()["end_reason"]=="batch_queue_full");CHECK(e.Status()["queued_batches"]==8);
  const auto log=Archive(directory/"current.jsonl");
  REQUIRE(log.size()==290);CHECK(log.back()["kind"]=="summary");
  auto next=LongStart(5);next["parameters"]["capture_id"]="00000000-0000-4000-8000-000000000008";
  CHECK(Act(e,next)["error_code"]=="delivery_pending");
  while(e.FrontBatch()) e.ConfirmBatch();
  CHECK(Act(e,next)["status"]=="completed");
  CHECK(std::filesystem::exists(directory/"previous.jsonl"));
  CHECK(e.Status()["transport_counters"]["frames_completed"]==288);
  CHECK(e.Status()["frames"]==0);
  e.TransportLost("serial_disconnected",wall);
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS rejected starts can retry and successful actions replay without restarting") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  auto request=LongStart();
  CHECK(Act(e,request,false)["error_code"]=="device_not_live");
  const auto first=Act(e,request);CHECK(first["status"]=="completed");
  request["command_id"]="00000000-0000-4000-8000-000000000009";
  auto replay=first;replay["command_id"]=request["command_id"];
  CHECK(Act(e,request)==replay);
  request["parameters"]["duration_seconds"]=1;
  CHECK(Act(e,request)["error_code"]=="action_conflict");
  CHECK(e.Busy());
  auto lower=Request("gnss_status",6);lower["lease_version"]=1;
  lower["session_id"]="00000000-0000-4000-8000-000000000009";
  CHECK(Act(e,lower)["error_code"]=="stale_lease");
  auto wrong=LongStart(7);wrong["target"]["device_id"]="DCDWCNS1OTHER";
  CHECK(Act(e,wrong)["error_code"]=="wrong_target");
  e.TransportLost("serial_disconnected",wall);
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS archive limit reserves a valid summary and never grows beyond 16MiB") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  std::vector<std::uint8_t> wire(1024);
  for(std::size_t i=0;i<wire.size();++i) wire[i]=static_cast<std::uint8_t>(i);
  for(unsigned i=1;i<=20000&&e.Busy();++i) {
    Feed(e,wire,0x758F,i,2);
    while(e.FrontBatch()) e.ConfirmBatch();
  }
  CHECK_FALSE(e.Busy());CHECK(e.Status()["end_reason"]=="archive_limit");
  CHECK(e.Status()["state"]=="failed");
  const auto bytes=std::filesystem::file_size(directory/"current.jsonl");
  CHECK(bytes<=16U*1024*1024);CHECK(bytes>16U*1024*1024-8192);
  const auto log=Archive(directory/"current.jsonl");REQUIRE(log.size()>5000);
  CHECK(log.back()["kind"]=="summary");CHECK(log.back()["gnss"]["end_reason"]=="archive_limit");
  CHECK(log.back()["gnss"]["frames"]==log.size()-2);
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS archive open rejection is retryable and retained capture IDs cannot be reused") {
  const auto directory=Directory();
  {std::ofstream blocker(directory);blocker<<"regular file";}
  {
    experiment::GnssExperiment e("gnsstest",{true,directory});
    CHECK(Act(e,LongStart())["error_code"]=="archive_open_failed");CHECK_FALSE(e.Busy());
    std::filesystem::remove(directory);
    CHECK(Act(e,LongStart())["status"]=="completed");
    CHECK(Act(e,Request("gnss_capture_stop",5))["status"]=="completed");
  }
  {
    experiment::GnssExperiment e("gnsstest",{true,directory});
    CHECK(Act(e,LongStart())["error_code"]=="capture_conflict");
    auto next=LongStart(6);next["parameters"]["capture_id"]="00000000-0000-4000-8000-000000000008";
    CHECK(Act(e,next)["status"]=="completed");
    e.TransportLost("serial_disconnected",wall);
  }
  {
    experiment::GnssExperiment e("gnsstest",{true,directory});
    CHECK(Act(e,LongStart())["error_code"]=="capture_conflict");
  }
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS disabled capture still permits bound offline status with bounded steady action retention") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{false,directory});
  CHECK(Act(e,LongStart())["error_code"]=="gnss_disabled");
  for(int i=10;i<138;++i) {
    const auto result=Act(e,Request("gnss_status",i),false);
    CHECK(result["status"]=="completed");CHECK(result["fact"]["gnss"]["enabled"]==false);
  }
  const auto payload=Request("gnss_status",138).dump();
  const auto at=[&](int elapsed) {
    auto result=e.Handle("gnsstest/"+id+"/experiment/set",payload,id,false,
        wall+std::chrono::seconds{elapsed},now+std::chrono::seconds{elapsed});
    REQUIRE(result.publication);return Json::parse(result.publication->payload);
  };
  CHECK(at(299)["error_code"]=="action_cache_full");CHECK(at(300)["status"]=="completed");
  CHECK_FALSE(std::filesystem::exists(directory));
}

TEST_CASE("GNSS captures use monotonic duration and accepted action expiry does not shorten capture") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,Request())["status"]=="completed");
  e.Tick(wall+std::chrono::hours{1000000},now+std::chrono::milliseconds{500},true);
  CHECK(e.Busy());
  e.Tick(wall-std::chrono::hours{100000},now+std::chrono::seconds{1},true);
  CHECK_FALSE(e.Busy());CHECK(e.Status()["end_reason"]=="duration_elapsed");
  std::filesystem::remove_all(directory);
}

TEST_CASE("GNSS ignores wire fragments received before capture start even when dequeued later") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  REQUIRE(Act(e,LongStart())["status"]=="completed");
  const std::vector<std::uint8_t> wire{'1','2','3','4','5','6','7','8','9'};
  Feed(e,wire,0x29B1,1,1,3,now-std::chrono::milliseconds{1});
  CHECK(e.Status()["frames"]==0);
  CHECK(e.Status()["transport_counters"]["fragments_received"]==0);
  Feed(e,wire,0x29B1,2);
  CHECK(e.Status()["frames"]==1);
  e.TransportLost("serial_disconnected",wall);
  REQUIRE(e.FrontBatch());
  const auto frame=Json::parse(e.FrontBatch()->payload)["frames"][0];
  CHECK(frame["elapsed_ms"]==0);CHECK(frame["source"]["frame_id"]==2);
  std::filesystem::remove_all(directory);
}
TEST_CASE("GNSS validates ownership and ends capture on transport loss") {
  const auto directory=Directory();
  experiment::GnssExperiment e("gnsstest",{true,directory});
  CHECK(Act(e,Request(),false)["error_code"]=="device_not_live");
  CHECK(Act(e,Request("gnss_capture_start",5))["status"]=="completed");
  auto stop=Request("gnss_capture_stop",6); stop["session_id"]="00000000-0000-4000-8000-000000000009";
  stop["lease_version"]=2;
  CHECK(Act(e,stop)["error_code"]=="capture_owner_mismatch"); CHECK(e.Busy());
  e.TransportLost("serial_disconnected",wall); CHECK_FALSE(e.Busy());
  CHECK(e.Status()["state"]=="failed"); CHECK(e.Status()["end_reason"]=="serial_disconnected");
  CHECK(Act(e,Request("gnss_status",7),false)["fact"]["gnss"]["state"]=="failed");
  std::filesystem::remove_all(directory);
}
