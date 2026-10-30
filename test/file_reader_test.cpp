#include "../components/storage/include/file_reader.h"
#include <array>
#include <cassert>
#include <cstring>
#include <unistd.h>
static void error(const char* actual,const char* expected) {
    assert(actual && std::strcmp(actual,expected)==0);
}
int main() {
    char directory[]="/tmp/storybox-reader.XXXXXX";
    assert(mkdtemp(directory));
    const std::string root=directory,path=root+"/测试.mp3";
    std::array<uint8_t,5000> bytes{};
    for (unsigned i=0;i<bytes.size();++i) bytes[i]=i%251;
    auto* file=std::fopen(path.c_str(),"wb"); assert(file);
    assert(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size()); assert(std::fclose(file)==0);
    Storage::AccessGate gate;
    Storage::FileReader reader(gate,root);
    Storage::FileListing listing;
    assert(!reader.list("/",0,64,listing,0));
    assert(listing.total==1 && listing.entries.size()==1 && listing.entries[0].size==5000);
    assert(!reader.list("/",1,64,listing,0) && listing.entries.empty() && listing.total==1);
    error(reader.list("测试.mp3",0,1,listing,0),"NOT_DIR");
    assert(gate.beginMaintenance(std::chrono::milliseconds(0)));
    error(reader.list("/",0,1,listing,0),"BUSY"); gate.endMaintenance();
    error(reader.begin("/测试.mp3",0),"NO_SESSION");
    assert(gate.acquire()); error(reader.beginSession(0),"AUDIO_BUSY"); gate.release();
    assert(!reader.beginSession(0)); assert(!gate.acquire());
    assert(!reader.list("/",0,1,listing,0)); // Works inside our own exclusive session.
    error(reader.beginSession(0),"BUSY");
    error(reader.begin("../outside",0),"BAD_PATH");
    error(reader.begin("missing",0),"NOT_FOUND");
    assert(!reader.begin("测试.mp3",1));
    const auto xid=reader.xid(); assert(xid && reader.size()==5000);
    error(reader.endSession(1),"BUSY");
    std::array<uint8_t,4096> output{}; std::size_t size=100;
    error(reader.read(xid+1,0,output.data(),output.size(),size,2),"BAD_XFER"); assert(!size);
    error(reader.read(xid,5001,output.data(),output.size(),size,2),"RANGE");
    error(reader.read(xid,0,output.data(),1,size,2),"BAD_LENGTH");
    assert(!reader.read(xid,0,output.data(),output.size(),size,3));
    assert(size==4096 && std::memcmp(output.data(),bytes.data(),size)==0);
    assert(!reader.read(xid,0,output.data(),output.size(),size,4)); // retry seeks back
    assert(size==4096 && std::memcmp(output.data(),bytes.data(),size)==0);
    assert(!reader.read(xid,4096,output.data(),output.size(),size,5));
    assert(size==904 && std::memcmp(output.data(),bytes.data()+4096,size)==0);
    assert(!reader.read(xid,5000,nullptr,0,size,6) && !size);
    reader.expire(30006); error(reader.end(xid,30006),"BAD_XFER");
    assert(!gate.acquire()); reader.expire(60006); assert(gate.acquire()); gate.release();
    assert(!reader.beginSession(60007));
    // Neither file nor intermediate directory symlinks may escape the root.
    const auto link=root+"/link";
    assert(symlink("/tmp",link.c_str())==0);
    error(reader.begin("link/outside",60008),"BAD_PATH");
    error(reader.begin("link",60008),"BAD_PATH");
    assert(!reader.begin("测试.mp3",60009)); assert(reader.xid()!=xid);
    // Truncation after stat/open is reported as IO, never a successful short chunk.
    assert(truncate(path.c_str(),0)==0);
    error(reader.read(reader.xid(),0,output.data(),output.size(),size,60010),"IO"); assert(!size);
    reader.abort(); assert(gate.acquire()); gate.release();
    assert(!reader.beginSession(60011)); assert(!reader.begin("测试.mp3",60012));
    assert(!reader.read(reader.xid(),0,nullptr,0,size,60013) && !size);
    reader.abort();
    Storage::FileReader writer(gate,root,true);
    assert(!writer.beginSession(70000));
    error(writer.beginWrite("../escape",3,true,70000),"BAD_PATH");
    error(writer.beginWrite("/.esp-story-index.bin",3,true,70000),"RESERVED_PATH");
    error(writer.beginWrite("测试.mp3",3,false,70000),"EXISTS");
    assert(!writer.beginWrite("测试.mp3",3,true,70000));
    auto upload=writer.xid();
    error(writer.begin("测试.mp3",70000),"BUSY");
    error(writer.endSession(70000),"BUSY");
    error(writer.endWrite(upload,70000),"INCOMPLETE");
    const uint8_t replacement[]={1,2,3},different[]={4,5,6};
    assert(!writer.write(upload,0,replacement,3,70001));
    assert(!writer.write(upload,0,replacement,3,70002));
    error(writer.write(upload,0,different,3,70002),"RANGE");
    assert(writer.written()==3 && !gate.acquire());
    assert(!writer.endWrite(upload,70003));
    assert(!writer.endWrite(upload,70004)); // Lost commit ACK is retryable.
    assert(!gate.acquire()); // Commit must not release the borrowed session lease.
    assert(!writer.begin("测试.mp3",70005));
    assert(!writer.read(writer.xid(),0,output.data(),output.size(),size,70005));
    assert(size==3 && std::memcmp(output.data(),replacement,3)==0);
    assert(!writer.end(writer.xid(),70006));
    assert(!writer.beginWrite("测试.mp3",3,true,70007));
    upload=writer.xid(); assert(!writer.write(upload,0,different,3,70008));
    assert(!writer.cancel(upload,70009));
    assert(!writer.beginWrite("测试.mp3",3,true,70010));
    assert(!writer.write(writer.xid(),0,different,3,70011));
    writer.expire(130011); // Timeout cancels only our temporary, retaining target.
    assert(gate.acquire()); gate.release();
    file=std::fopen(path.c_str(),"rb"); assert(file);
    assert(std::fread(output.data(),1,3,file)==3 && std::memcmp(output.data(),replacement,3)==0);
    assert(std::fclose(file)==0);
    assert(access((path+".storybox-upload").c_str(),F_OK)!=0);
    assert(!writer.beginSession(140000));
    assert(!writer.beginWrite("empty.bin",0,false,140000));
    assert(!writer.endWrite(writer.xid(),140000));
    assert(!writer.begin("empty.bin",140000));
    assert(writer.size()==0);
    assert(!writer.end(writer.xid(),140000));
    assert(unlink((root+"/empty.bin").c_str())==0);
    assert(std::rename(path.c_str(),(path+".storybox-backup").c_str())==0);
    file=std::fopen((path+".storybox-upload").c_str(),"wb"); assert(file);
    assert(std::fwrite(different,1,3,file)==3); assert(std::fclose(file)==0);
    assert(!writer.recover("测试.mp3",140001));
    assert(access((path+".storybox-interrupted").c_str(),F_OK)==0);
    file=std::fopen(path.c_str(),"rb"); assert(file);
    assert(std::fread(output.data(),1,3,file)==3 && std::memcmp(output.data(),replacement,3)==0);
    assert(std::fclose(file)==0);
    // Both versions present is ambiguous: neither may be removed or replaced.
    file=std::fopen((path+".storybox-backup").c_str(),"wb"); assert(file); assert(std::fclose(file)==0);
    error(writer.recover("测试.mp3",140002),"RECOVERY_REQUIRED");
    writer.abort();
    assert(unlink((path+".storybox-backup").c_str())==0);
    assert(unlink((path+".storybox-interrupted").c_str())==0);
    assert(unlink((root+"/.esp-story-index.dirty").c_str())==0);
    assert(unlink(link.c_str())==0); assert(unlink(path.c_str())==0); assert(rmdir(directory)==0);
}
