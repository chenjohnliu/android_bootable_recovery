"""Run the actual native operation bodies against temporary host files/faults."""
from pathlib import Path
import argparse
import subprocess
import tempfile
import xml.etree.ElementTree as ET

parser=argparse.ArgumentParser()
default=Path(__file__).resolve().parents[1]
if not (default/'m11qEssentials.cpp').is_file(): default=Path('/home/quokka/twrp/bootable/recovery')
parser.add_argument('--recovery-root',type=Path,default=default)
repo=parser.parse_args().recovery_root
source=(repo/'m11qEssentials.cpp').read_text()
def function(marker):
    start=source.index(marker); opening=source.index('{',start); depth=1; end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]

support=r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
using std::string;
using Bytes=std::vector<uint8_t>;
string suite,target;
bool unlocked=true,hashtree=true,storage=true,valid=true,ramdisk=true,pins=true;
int flags=0,installer=0,sessions=0,snapshots=0,saves=0,writes=0,installs=0;
int writeFailures=0,renameFailure=0,renames=0;
bool Fail(const string&) { return false; }
template<typename... T> void gui_print(const char*,T...) {}
struct Fd { int n; explicit Fd(int value):n(value){} ~Fd(){if(n>=0)close(n);} };
bool Regular(const string& p) { struct stat s;return !lstat(p.c_str(),&s)&&S_ISREG(s.st_mode); }
bool Exists(const string& p) { struct stat s;return !lstat(p.c_str(),&s); }
bool Read(const string& path,Bytes& bytes,size_t=128*1024*1024) {
  if(path=="/addon/magisk-30.7.zip") {bytes={10};return true;}
  if(path=="/addon/quokka-magisk.zip") {bytes={11};return true;}
  if(path=="/addon/quokka-magisk-updater.sh") {bytes={12};return true;}
  std::ifstream file(path,std::ios::binary);
  if(!file)return false;
  bytes.assign(std::istreambuf_iterator<char>(file),{});return !bytes.empty();
}
string Hash(const Bytes& b) {
  if(b==Bytes{10})return pins?"e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5":"bad";
  if(b==Bytes{11})return "df72610d10110db58ca3b70dd563f96b56ab42569f24734c57afc29cfdebd065";
  if(b==Bytes{12})return "84157455f4099214c70a6fc1044e45926c2f8e6d50e77c1ac88b1e251ef70643";
  return string(b.begin(),b.end());
}
bool WriteBytes(int fd,const uint8_t* bytes,size_t size,off_t offset=0) {
  ++writes;
  if(writeFailures>0){--writeFailures;return false;}
  return pwrite(fd,bytes,size,offset)==ssize_t(size)&&fsync(fd)==0;
}
void file(const string& p,const Bytes& b) { std::ofstream f(p,std::ios::binary);f.write((const char*)b.data(),b.size()); }
bool Text(const string& p,string& s) { Bytes b;if(!Read(p,b))return false;s=string(b.begin(),b.end());return true; }
bool UnlockedForBootEdit(){return unlocked;}
bool RomHashtreeDisabled(){return hashtree;}
bool Block(const string& name,string& p){p=suite+"/"+name;return true;}
bool ReadBoot(string& p,Bytes& b){p=target;return Read(p,b);}
bool Session(const string&,string& p){++sessions;if(!storage)return false;p=suite+"/backup";return true;}
bool Space(const string&,size_t){return storage;}
bool VerifiedSave(const string&,const Bytes&){++saves;return storage;}
bool BootSnapshot(const string&,const Bytes&,const string&){++snapshots;return storage;}
bool Save(const string&,const string&){++saves;return storage;}
string Fingerprint(){return "host-rom";}
namespace m11q {
bool AvbFlags(const Bytes&,uint32_t& f){f=flags;return valid;}
bool PrepareAvb(const Bytes& old,Bytes& changed){changed=old;changed[0]=3;return valid;}
uint32_t Le32(const Bytes&,size_t){return ramdisk?1:0;}
bool ValidBoot(const Bytes& b,string&){return b.size()==64&&b[0]==42;}
}
struct Manager { string Get_Android_Root_Path(){return suite+"/system";} } PartitionManager;
struct RomMount {
 string point;
 bool Open(const string& p,bool=false){point=p=="/vendor"?suite+"/vendor":p;return true;}
 string Path()const{return point;}
};
int hostRename(const char* from,const char* to) {
 ++renames;if(renameFailure==renames){errno=EIO;return -1;}return std::rename(from,to);
}
int TWinstall_zip(const string&,int*,bool) {
 ++installs;assert(string(getenv("QUOKKA_ESSENTIALS_INSTALL"))=="1");
 Bytes b(64,43);if(installer!=2)b[0]=42;file(target,b);return installer==1?1:0;
}
void reset() {
 std::filesystem::remove_all(suite+"/system");std::filesystem::remove_all(suite+"/vendor");
 std::filesystem::create_directory(suite+"/system");std::filesystem::create_directory(suite+"/vendor");
 file(target,Bytes(64,42));file(suite+"/vbmeta",Bytes(32,0));
 file(suite+"/system/recovery-from-boot.p",Bytes(20,7));
 file(suite+"/vendor/recovery-from-boot.p",Bytes(20,8));
 unlocked=hashtree=valid=ramdisk=pins=true;storage=false;flags=installer=0;
 sessions=snapshots=saves=writes=installs=writeFailures=renameFailure=renames=0;
}
#define rename hostRename
'''
main=r'''
#undef rename
int main() {
 char location[]="/tmp/quokka-backup-test-XXXXXX";suite=mkdtemp(location);assert(!suite.empty());target=suite+"/boot";
 // No storage: original routes abort, explicit skip routes complete.
 reset();assert(!PrepareAvb());assert(sessions==1&&writes==0);
 reset();assert(PrepareAvb(false));assert(sessions==0&&saves==0&&writes==1);
 reset();unlocked=false;assert(!PrepareAvb(false));assert(writes==0);
 reset();valid=false;assert(!PrepareAvb(false));assert(writes==0);
 reset();flags=3;assert(PrepareAvb(false));assert(writes==0&&sessions==0);
 reset();writeFailures=1;assert(!PrepareAvb(false));Bytes b;assert(Read(suite+"/vbmeta",b)&&b==Bytes(32,0));assert(writes==2);
 reset();storage=true;assert(PrepareAvb());assert(sessions==1&&snapshots==1&&saves==4&&writes==1);
 reset();assert(!PreventStockRestore());assert(renames==0);
 reset();assert(PreventStockRestore(false));assert(sessions==0&&saves==0&&renames==2);
 assert(Regular(suite+"/system/recovery-from-boot.p.quokka-disabled"));
 reset();hashtree=false;assert(!PreventStockRestore(false));assert(renames==0);
 reset();renameFailure=2;assert(!PreventStockRestore(false));
 assert(Regular(suite+"/system/recovery-from-boot.p")&&Regular(suite+"/vendor/recovery-from-boot.p"));
 reset();storage=true;assert(PreventStockRestore());assert(sessions==1&&saves==3&&renames==2);
 reset();assert(!MagiskInstall());assert(installs==0);
 reset();assert(MagiskInstall(false));assert(sessions==0&&saves==0&&installs==1);
 reset();unlocked=false;assert(!MagiskInstall(false));assert(installs==0);
 reset();pins=false;assert(!MagiskInstall(false));assert(installs==0);
 reset();ramdisk=false;assert(!MagiskInstall(false));assert(installs==0);
 reset();installer=1;assert(!MagiskInstall(false));assert(Read(target,b)&&b==Bytes(64,42));assert(writes==1);
 reset();installer=2;assert(!MagiskInstall(false));assert(Read(target,b)&&b==Bytes(64,42));assert(writes==1);
 reset();storage=true;assert(MagiskInstall());assert(sessions==1&&snapshots==1&&saves==1);
 assert(getenv("QUOKKA_ESSENTIALS_INSTALL")==nullptr);
 reset();setenv("QUOKKA_ESSENTIALS_INSTALL","previous",1);assert(MagiskInstall(false));
 assert(string(getenv("QUOKKA_ESSENTIALS_INSTALL"))=="previous");unsetenv("QUOKKA_ESSENTIALS_INSTALL");
 std::filesystem::remove_all(suite);
 std::cout<<"21 native fault/backup cases passed using actual operation bodies.\n";
}
'''
cpp=support+'\n\n'.join(function(marker) for marker in (
    'bool Flash(', 'bool PrepareAvb(', 'bool PreventStockRestore(', 'bool MagiskInstall('))+main
with tempfile.TemporaryDirectory(prefix='quokka-native-host-') as temp:
    path=Path(temp)/'test.cpp'; path.write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path),'-o',str(Path(temp)/'test')],check=True)
    subprocess.run([str(Path(temp)/'test')],check=True)

# Interpret actual set/page actions to check suffix resolution, cancellation and no sticky skip mode.
theme=ET.parse(repo/'gui/theme/common/portrait.xml').getroot()
choice=theme.find('./pages/page[@name="m11q_backup_choice"]')
assert choice is not None
buttons={b.findtext('text'):b for b in choice.findall('button')}
def actions(button,state):
    destination=None
    for action in button.findall('./actions/action'):
        value=action.text or ''
        for key,data in list(state.items()):value=value.replace('%'+key+'%',data)
        if action.attrib['function']=='set':
            key,data=value.split('=',1);state[key]=data
        elif action.attrib['function']=='page':destination=value
    return destination
for page_name,label,op in [('m11q_essentials','Keep TWRP','stock-restore'),
                           ('m11q_essentials','Magisk 30.7','magisk-install'),
                           ('m11q_avb','Prepare AVB','avb-prepare')]:
    original=next(b for b in theme.find('./pages/page[@name="'+page_name+'"]').findall('button') if b.findtext('text')==label)
    state={}
    assert actions(original,state)=='m11q_backup_choice'
    assert actions(buttons['Skip backup'],state)=='confirm_action'
    assert state['tw_action_param']==op+'-no-backup' and state['tw_slider_text']=='Swipe without Backup'
    assert actions(original,state)=='m11q_backup_choice' and state['tw_action_param']==op
    assert actions(buttons['Back up first'],state)=='confirm_action' and state['tw_action_param']==op
    assert 'No backup' not in state['tw_text2']
    assert actions(buttons['Back'],state)==page_name
    assert 'operation == "'+op+'-no-backup"' in source
print('UI route interpolation, per-operation reset, back navigation and explicit swipe passed.')
