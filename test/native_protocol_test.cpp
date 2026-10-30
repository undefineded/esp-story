#include "native_protocol.h"
#include "cJSON.h"
#include "file_reader.h"
#include "../src/transfer_frame.h"
#include <cassert>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
static cJSON* request(const std::string& text) {
    char response[1024];
    assert(Host::respond(reinterpret_cast<const uint8_t*>(text.data()),text.size(),123,456,response,sizeof(response)));
    auto* value=cJSON_Parse(response);assert(value);return value;
}
static void error(const std::string& input,const char* expected) {
    auto* value=request(input);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(value,"ok")));
    assert(std::strcmp(cJSON_GetObjectItem(value,"err")->valuestring,expected)==0);
    cJSON_Delete(value);
}
int main() {
    auto* hello=request("{\"v\":4,\"id\":7,\"cmd\":\"sys\",\"act\":\"hello\"}");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(hello,"ok")));
    assert(cJSON_GetObjectItem(hello,"id")->valueint==7 && cJSON_GetObjectItem(hello,"proto")->valueint==4);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(hello,"features"))==0);
    cJSON_Delete(hello);
    auto* ping=request("{\"v\":4,\"cmd\":\"sys\",\"act\":\"ping\"}");
    assert(cJSON_GetObjectItem(ping,"uptime")->valueint==123 && cJSON_GetObjectItem(ping,"heap")->valueint==456);
    cJSON_Delete(ping);
    error("{}","PROTOCOL_V4_REQUIRED");
    error("{\"v\":2}","PROTOCOL_V4_REQUIRED");
    error("{\"v\":4,\"cmd\":\"fs\"}","UNSUPPORTED_FEATURE");
    error("{\"v\":4,\"cmd\":\"sys\",\"act\":\"unknown\"}","BAD_ACT");
    error("{}garbage","BAD_JSON");
    error(std::string(20,'[')+std::string(20,']'),"BAD_JSON");
    error(std::string("{}\0",3),"BAD_JSON");
    error(std::string(1025,' '),"BAD_JSON");
    error("{\"v\":4,\"cmd\":\"hello\\u0000fs\"}","BAD_JSON");
    error("{\"v\":4,\"v\":2,\"cmd\":\"hello\"}","BAD_JSON");
    char tiny[2];const char* input="{\"v\":4,\"cmd\":\"hello\"}";
    assert(!Host::respond(reinterpret_cast<const uint8_t*>(input),std::strlen(input),0,0,tiny,sizeof(tiny)));
    char directory[]="/tmp/storybox-protocol-files.XXXXXX";
    assert(mkdtemp(directory));
    const auto path=std::string(directory)+"/sample.mp3";
    auto* file=std::fopen(path.c_str(),"wb"); assert(file);
    std::string payload(5000,'x'); payload[3]='\0'; payload[4999]='Z';
    assert(std::fwrite(payload.data(),1,payload.size(),file)==payload.size()); assert(std::fclose(file)==0);
    Storage::AccessGate gate;
    Storage::FileReader files(gate,directory);
    uint8_t raw[4096]; std::size_t raw_size=0;
    char json[4096];
    auto call=[&](const std::string& body,const char* expected=nullptr,std::size_t input_raw=0) {
        const auto text="{\"v\":4,\"id\":9,"+body+"}";
        assert(Host::respond(reinterpret_cast<const uint8_t*>(text.data()),text.size(),123,456,json,sizeof(json),
            &files,raw,sizeof(raw),&raw_size,input_raw,123));
        auto* result=cJSON_Parse(json); assert(result);
        assert(cJSON_GetObjectItem(result,"id")->valueint==9);
        if (expected) {
            assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"ok")));
            assert(std::strcmp(cJSON_GetObjectItem(result,"err")->valuestring,expected)==0);
            assert(raw_size==0);
        } else assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok")));
        cJSON_Delete(result);
    };
    call("\"cmd\":\"hello\"");
    assert(std::strstr(json,"fs_readonly"));
    call("\"cmd\":\"fs\",\"act\":\"list\",\"path\":\"/\",\"offset\":-1","BAD_ARG");
    call("\"cmd\":\"fs\",\"act\":\"list\",\"path\":\"/\"");
    assert(std::strstr(json,"sample.mp3") && std::strstr(json,"\"readonly\":true"));
    call("\"cmd\":\"fs\",\"act\":\"read_begin\",\"path\":\"sample.mp3\"","NO_SESSION");
    call("\"cmd\":\"fs\",\"act\":\"session_begin\"");
    assert(!gate.acquire());
    call("\"cmd\":\"fs\",\"act\":\"write_begin\",\"path\":\"sample.mp3\"","UNSUPPORTED_FEATURE");
    call("\"cmd\":\"fs\",\"act\":\"read_begin\",\"path\":\"sample.mp3\"");
    assert(files.xid()==1 && files.size()==5000);
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":-1","BAD_ARG");
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":0.5","BAD_ARG");
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":4294967296","BAD_ARG");
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":0","BAD_LENGTH",1);
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":0");
    assert(raw_size==4096 && std::memcmp(raw,payload.data(),raw_size)==0);
    // Exercise real SBX4 framing with binary NULs and production CRC/parser.
    uint8_t frame[8192]; const auto json_size=std::strlen(json);
    std::memcpy(frame+TransferFrame::Header,json,json_size);
    std::memcpy(frame+TransferFrame::Header+json_size,raw,raw_size);
    TransferFrame::header(frame,json_size,raw_size);
    TransferFrame::Decoder decoder;
    const auto frame_size=TransferFrame::Header+json_size+raw_size;
    for (std::size_t i=0;i<frame_size;++i) assert(decoder.feed(frame[i])==(i+1==frame_size));
    assert(TransferFrame::u16(decoder.bytes+6)==4096);
    call("\"cmd\":\"fs\",\"act\":\"read_chunk\",\"xid\":1,\"offset\":4096");
    assert(raw_size==904 && std::memcmp(raw,payload.data()+4096,raw_size)==0);
    call("\"cmd\":\"fs\",\"act\":\"read_end\",\"xid\":1");
    call("\"cmd\":\"fs\",\"act\":\"session_end\"");
    assert(gate.acquire()); gate.release();
    // Long quoted names force byte-bounded short pages; all entries must still
    // be reachable by advancing offset by returned, with no empty middle page.
    std::vector<std::string> extra;
    for (unsigned i=0;i<16;++i) {
        extra.push_back(std::string(directory)+"/"+std::to_string(i)+std::string(170,'\"'));
        auto* f=std::fopen(extra.back().c_str(),"wb"); assert(f); assert(std::fclose(f)==0);
    }
    unsigned offset=0,pages=0;
    do {
        call("\"cmd\":\"fs\",\"act\":\"list\",\"path\":\"/\",\"limit\":96,\"offset\":"+std::to_string(offset));
        auto* result=cJSON_Parse(json); assert(result);
        auto* data=cJSON_GetObjectItem(result,"data");
        const auto returned=cJSON_GetObjectItem(data,"returned")->valueint;
        assert(returned>0 && cJSON_GetObjectItem(data,"total")->valueint==17);
        assert(returned==cJSON_GetArraySize(cJSON_GetObjectItem(data,"entries")));
        offset+=returned; ++pages; cJSON_Delete(result);
    } while (offset<17);
    assert(offset==17 && pages>2);
    for (const auto& name:extra) assert(unlink(name.c_str())==0);
    Storage::FileReader writable(gate,directory,true);
    auto writeCall=[&](const std::string& body,const uint8_t* bytes=nullptr,std::size_t count=0,const char* error=nullptr) {
        const auto text="{\"v\":4,\"cmd\":\"fs\","+body+"}";
        assert(Host::respond(reinterpret_cast<const uint8_t*>(text.data()),text.size(),123,456,json,sizeof(json),
            &writable,raw,sizeof(raw),&raw_size,count,123,bytes));
        auto* result=cJSON_Parse(json); assert(result);
        if (error) assert(std::strcmp(cJSON_GetObjectItem(result,"err")->valuestring,error)==0);
        else assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok")));
        assert(raw_size==0); cJSON_Delete(result);
    };
    writeCall("\"act\":\"session_begin\"");
    writeCall("\"act\":\"write_begin\",\"path\":\"sample.mp3\",\"size\":5000,\"overwrite\":false",nullptr,0,"EXISTS");
    writeCall("\"act\":\"write_begin\",\"path\":\"sample.mp3\",\"size\":5000");
    writeCall("\"act\":\"write_chunk\",\"xid\":1,\"offset\":0",nullptr,0,"BAD_LENGTH");
    writeCall("\"act\":\"write_chunk\",\"xid\":1,\"offset\":0",reinterpret_cast<const uint8_t*>(payload.data()),4096);
    writeCall("\"act\":\"write_end\",\"xid\":1",nullptr,0,"INCOMPLETE");
    writeCall("\"act\":\"write_chunk\",\"xid\":1,\"offset\":4096",reinterpret_cast<const uint8_t*>(payload.data()+4096),904);
    writeCall("\"act\":\"write_end\",\"xid\":1");
    writeCall("\"act\":\"write_end\",\"xid\":1");
    writeCall("\"act\":\"session_end\"");
    assert(unlink((std::string(directory)+"/.esp-story-index.dirty").c_str())==0);
    assert(unlink(path.c_str())==0); assert(rmdir(directory)==0);
}
