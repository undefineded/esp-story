#include "native_protocol.h"
#include "file_reader.h"
#include "cJSON.h"
#include <cstring>
#include <cmath>
#include <limits>
namespace Host {
namespace {
bool bounded(const uint8_t* input,std::size_t size) {
    if (!size || size>1024) return false;
    bool quoted=false,escape=false; int depth=0;
    for (std::size_t i=0;i<size;++i) {
        const auto c=input[i];
        if (!c) return false;
        if (quoted) {
            if (escape) {
                // cJSON strings are NUL-terminated: an escaped NUL must not
                // silently truncate a command, action or filesystem path.
                if (c=='u' && i+4<size && std::memcmp(input+i+1,"0000",4)==0) return false;
                escape=false;
            }
            else if (c=='\\') escape=true;
            else if (c=='"') quoted=false;
        } else if (c=='"') quoted=true;
        else if (c=='{'||c=='[') { if (++depth>16) return false; }
        else if (c=='}'||c==']') { if (--depth<0) return false; }
    }
    return !quoted && depth==0;
}
const char* string(cJSON* document,const char* name,const char* fallback="") {
    const auto* item=cJSON_GetObjectItemCaseSensitive(document,name);
    return cJSON_IsString(item)?item->valuestring:fallback;
}
bool unsignedNumber(cJSON* request,const char* name,uint32_t& value) {
    const auto* item=cJSON_GetObjectItemCaseSensitive(request,name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble<0 || item->valuedouble>UINT32_MAX ||
        std::floor(item->valuedouble)!=item->valuedouble) return false;
    value=static_cast<uint32_t>(item->valuedouble); return true;
}
const char* fileRequest(cJSON* request,cJSON* response,Storage::FileReader& files,
                       uint8_t* raw,std::size_t capacity,std::size_t& length,uint64_t now,
                       const uint8_t* input_raw,std::size_t input_length) {
    const char* action=string(request,"act");
    cJSON_AddStringToObject(response,"cmd","fs");
    cJSON_AddStringToObject(response,"act",action);
    if (std::strcmp(action,"session_begin")==0) return files.beginSession(now);
    if (std::strcmp(action,"session_end")==0) return files.endSession(now);
    if (files.writable() && std::strcmp(action,"recover")==0)
        return files.recover(string(request,"path"),now);
    if (files.writable() && std::strcmp(action,"write_begin")==0) {
        uint32_t size=0;
        if (!unsignedNumber(request,"size",size)) return "BAD_ARG";
        const auto* overwrite=cJSON_GetObjectItemCaseSensitive(request,"overwrite");
        if (overwrite && !cJSON_IsBool(overwrite)) return "BAD_ARG";
        if (const auto* error=files.beginWrite(string(request,"path"),size,!overwrite || cJSON_IsTrue(overwrite),now)) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddNumberToObject(data,"xid",files.xid());
        cJSON_AddStringToObject(data,"path",files.path().c_str());
        cJSON_AddNumberToObject(data,"size",files.size());
        cJSON_AddNumberToObject(data,"chunk",4096);
        return nullptr;
    }
    if (files.writable() && std::strcmp(action,"write_chunk")==0) {
        uint32_t xid=0,offset=0;
        if (!unsignedNumber(request,"xid",xid) || !unsignedNumber(request,"offset",offset)) return "BAD_ARG";
        if (!input_raw || !input_length || input_length>4096) return "BAD_LENGTH";
        if (const auto* error=files.write(xid,offset,input_raw,input_length,now)) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddNumberToObject(data,"xid",xid);
        cJSON_AddNumberToObject(data,"written",files.written());
        return nullptr;
    }
    if (files.writable() && std::strcmp(action,"write_end")==0) {
        uint32_t xid=0;
        if (!unsignedNumber(request,"xid",xid)) return "BAD_ARG";
        if (const auto* error=files.endWrite(xid,now)) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddStringToObject(data,"path",files.committedPath().c_str());
        cJSON_AddNumberToObject(data,"size",files.committedSize());
        cJSON_AddNumberToObject(data,"expected",files.committedSize());
        cJSON_AddBoolToObject(data,"complete",true);
        return nullptr;
    }
    if (std::strcmp(action,"list")==0) {
        uint32_t offset=0,limit=16;
        if ((cJSON_HasObjectItem(request,"offset") && !unsignedNumber(request,"offset",offset)) ||
            (cJSON_HasObjectItem(request,"limit") && !unsignedNumber(request,"limit",limit))) return "BAD_ARG";
        Storage::FileListing listing;
        if (const auto* error=files.list(string(request,"path","/"),offset,limit,listing,now)) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddStringToObject(data,"path",listing.path.c_str());
        const auto parent=listing.path=="/"?"":listing.path.substr(0,listing.path.find_last_of('/'));
        cJSON_AddStringToObject(data,"parent",listing.path!="/" && parent.empty()?"/":parent.c_str());
        cJSON_AddNumberToObject(data,"offset",offset);
        cJSON_AddNumberToObject(data,"limit",listing.limit);
        cJSON_AddNumberToObject(data,"total",listing.total);
        cJSON_AddNumberToObject(data,"returned",listing.entries.size());
        cJSON_AddBoolToObject(data,"ready",true);
        cJSON_AddBoolToObject(data,"readonly",!files.writable());
        // Capacity/used/free are omitted until real card geometry is supplied.
        auto* entries=cJSON_AddArrayToObject(data,"entries");
        for (const auto& entry:listing.entries) {
            auto* value=cJSON_CreateObject();
            cJSON_AddStringToObject(value,"name",entry.name.c_str());
            cJSON_AddStringToObject(value,"path",entry.path.c_str());
            cJSON_AddBoolToObject(value,"dir",entry.directory);
            cJSON_AddNumberToObject(value,"size",entry.size);
            cJSON_AddNumberToObject(value,"mtime",entry.mtime);
            cJSON_AddItemToArray(entries,value);
        }
        return nullptr;
    }
    if (std::strcmp(action,"read_begin")==0) {
        const auto* error=files.begin(string(request,"path"),now);
        if (error) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddNumberToObject(data,"xid",files.xid());
        cJSON_AddStringToObject(data,"path",files.path().c_str());
        cJSON_AddStringToObject(data,"name",files.path().c_str()+files.path().find_last_of('/')+1);
        cJSON_AddNumberToObject(data,"size",files.size());
        cJSON_AddNumberToObject(data,"chunk",4096);
        return nullptr;
    }
    if (std::strcmp(action,"read_chunk")==0) {
        uint32_t xid=0,offset=0;
        if (!unsignedNumber(request,"xid",xid) || !unsignedNumber(request,"offset",offset)) return "BAD_ARG";
        const auto* error=files.read(xid,offset,raw,capacity,length,now);
        if (error) return error;
        auto* data=cJSON_AddObjectToObject(response,"data");
        cJSON_AddNumberToObject(data,"xid",xid);
        cJSON_AddNumberToObject(data,"offset",offset);
        cJSON_AddNumberToObject(data,"len",length);
        cJSON_AddNumberToObject(data,"size",files.size());
        cJSON_AddBoolToObject(data,"eof",uint64_t(offset)+length>=files.size());
        return nullptr;
    }
    if (std::strcmp(action,"read_end")==0 || std::strcmp(action,"cancel")==0) {
        uint32_t xid=0;
        if (!unsignedNumber(request,"xid",xid)) return "BAD_ARG";
        return std::strcmp(action,"cancel")==0 ? files.cancel(xid,now) : files.end(xid,now);
    }
    return "UNSUPPORTED_FEATURE"; // No writes advertised or accepted yet.
}
}
std::size_t respond(const uint8_t* input,std::size_t size,uint32_t uptime,uint32_t heap,
                    char* output,std::size_t capacity,Storage::FileReader* files,uint8_t* raw,
                    std::size_t raw_capacity,std::size_t* raw_size,std::size_t input_raw_size,
                    uint64_t monotonic_ms,const uint8_t* input_raw) {
    if (raw_size) *raw_size=0;
    std::size_t length=0;
    if (!output || capacity>std::size_t(std::numeric_limits<int>::max())) return 0;
    cJSON* request=nullptr;
    const char* end=nullptr;
    if (input && bounded(input,size)) {
        request=cJSON_ParseWithLengthOpts(reinterpret_cast<const char*>(input),size,&end,false);
        if (request) {
            const auto* limit=reinterpret_cast<const char*>(input)+size;
            while (end<limit && (*end==' '||*end=='\r'||*end=='\n'||*end=='\t')) ++end;
            bool duplicate=false;
            for (auto* item=request->child;item && !duplicate;item=item->next)
                for (auto* other=item->next;other;other=other->next)
                    if (item->string && other->string && std::strcmp(item->string,other->string)==0)
                        { duplicate=true; break; }
            if (end!=limit || !cJSON_IsObject(request) || duplicate) { cJSON_Delete(request); request=nullptr; }
        }
    }
    cJSON* response=cJSON_CreateObject();
    if (!response) { cJSON_Delete(request); return 0; }
    cJSON_AddNumberToObject(response,"v",4);
    const auto* id=cJSON_GetObjectItemCaseSensitive(request,"id");
    if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) &&
        id->valuedouble>=INT32_MIN && id->valuedouble<=INT32_MAX && std::floor(id->valuedouble)==id->valuedouble)
        cJSON_AddNumberToObject(response,"id",id->valuedouble);
    const auto* version=cJSON_GetObjectItemCaseSensitive(request,"v");
    const char* error=nullptr;
    if (!request) error="BAD_JSON";
    else if (!cJSON_IsNumber(version) || version->valuedouble!=4) error="PROTOCOL_V4_REQUIRED";
    else if (input_raw_size && (!files || !files->writable() ||
        std::strcmp(string(request,"cmd"),"fs")!=0 || std::strcmp(string(request,"act"),"write_chunk")!=0)) error="BAD_LENGTH";
    else {
        const auto* cmd=string(request,"cmd");
        const bool hello=std::strcmp(cmd,"hello")==0;
        const auto* action=string(request,"act",hello?"hello":"");
        if (std::strcmp(cmd,"fs")==0 && files)
            error=fileRequest(request,response,*files,raw,raw_size?raw_capacity:0,length,monotonic_ms,input_raw,input_raw_size);
        else if (!hello && std::strcmp(cmd,"sys")!=0) error="UNSUPPORTED_FEATURE";
        else if (std::strcmp(action,"hello")==0) {
            cJSON_AddStringToObject(response,"evt","hello");
            cJSON_AddStringToObject(response,"fw","storybox-idf-migration");
            cJSON_AddNumberToObject(response,"proto",4);
            cJSON_AddStringToObject(response,"board","ESP32-S3");
            cJSON_AddStringToObject(response,"name","StoryBox-IDF");
            cJSON_AddStringToObject(response,"role","storybox");
            auto* features=cJSON_AddArrayToObject(response,"features");
            if (files) cJSON_AddItemToArray(features,cJSON_CreateString(files->writable()?"fs_transfer":"fs_readonly"));
        } else if (std::strcmp(action,"ping")==0) {
            cJSON_AddStringToObject(response,"evt","pong");
            cJSON_AddNumberToObject(response,"uptime",uptime);
            cJSON_AddNumberToObject(response,"heap",heap);
        } else error="BAD_ACT";
    }
    cJSON_AddBoolToObject(response,"ok",!error);
    if (error) cJSON_AddStringToObject(response,"err",error);
    bool written=cJSON_PrintPreallocated(response,output,capacity,false);
    // Directory pages are bounded by bytes as well as entry count. Short pages
    // retain total/offset, so clients advance by returned, not requested limit.
    if (!written && !error && std::strcmp(string(request,"cmd"),"fs")==0 &&
        std::strcmp(string(request,"act"),"list")==0) {
        auto* data=cJSON_GetObjectItemCaseSensitive(response,"data");
        auto* entries=cJSON_GetObjectItemCaseSensitive(data,"entries");
        while (!written && cJSON_GetArraySize(entries)>1) {
            cJSON_DeleteItemFromArray(entries,cJSON_GetArraySize(entries)-1);
            cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"returned"),cJSON_GetArraySize(entries));
            written=cJSON_PrintPreallocated(response,output,capacity,false);
        }
    }
    cJSON_Delete(request); cJSON_Delete(response);
    if (written && !error && raw_size) *raw_size=length;
    return written?std::strlen(output):0;
}
}
