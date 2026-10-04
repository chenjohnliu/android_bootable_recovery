// SPDX-License-Identifier: GPL-3.0-or-later
// Manual, device-scoped recovery tools. No boot-time hooks or automatic reboot.
#include "m11qEssentials.hpp"
#include "data.hpp"
#include "gui/gui.hpp"
#include "m11qBootHeader.hpp"
#include "m11qModuleState.hpp"
#include "m11qAvb.hpp"
#include "partitions.hpp"
#include "twinstall.h"
#include "twrp-functions.hpp"
#include "variables.h"
#include <algorithm>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <memory>
#include <openssl/sha.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
using std::string;
using Bytes = std::vector<uint8_t>;
struct Fd {
  int n;
  explicit Fd(int v) : n(v) {}
  ~Fd() {
    if (n >= 0)
      close(n);
  }
};
bool Fail(const string &message) {
  gui_print("ERROR: %s\n", message.c_str());
  return false;
}
string Hash(const uint8_t *data, size_t size) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(data, size, digest);
  static const char *hex = "0123456789abcdef";
  string s;
  for (auto c : digest) {
    s += hex[c >> 4];
    s += hex[c & 15];
  }
  return s;
}
string Hash(const Bytes &b) { return Hash(b.data(), b.size()); }
string Hash(const string &s) {
  return Hash(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
bool SafeId(const string &s) { return m11q::SafeId(s); }
bool Exists(const string &p) {
  struct stat s;
  return lstat(p.c_str(), &s) == 0;
}
bool Regular(const string &p) {
  struct stat s;
  return lstat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode);
}
bool Directory(const string &p) {
  struct stat s;
  return lstat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}
bool Read(const string &path, Bytes &b, size_t limit = 128 * 1024 * 1024) {
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat s;
  if (fd.n < 0 || fstat(fd.n, &s) ||
      (!S_ISREG(s.st_mode) && !S_ISBLK(s.st_mode)))
    return Fail("Cannot read " + path);
  uint64_t size = s.st_size;
  if (S_ISBLK(s.st_mode) && ioctl(fd.n, BLKGETSIZE64, &size))
    return Fail("Cannot get block capacity");
  if (!size || size > limit)
    return Fail("Unexpected file/block size: " + path);
  b.resize(size);
  size_t offset = 0;
  while (offset < size) {
    ssize_t n = read(fd.n, b.data() + offset, size - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return Fail("Short read: " + path);
    offset += n;
  }
  return true;
}
bool WriteBytes(int fd, const uint8_t *p, size_t size, off_t offset = 0) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = pwrite(fd, p + done, size - done, offset + done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += n;
  }
  return fsync(fd) == 0;
}
bool Save(const string &path, const uint8_t *p, size_t size) {
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             0600));
  if (fd.n < 0 || !WriteBytes(fd.n, p, size))
    return Fail("Cannot save " + path);
  return true;
}
bool Save(const string &path, const Bytes &b) {
  return Save(path, b.data(), b.size());
}
bool Save(const string &path, const string &s) {
  return Save(path, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
bool Text(const string &p, string &s) {
  return Regular(p) && android::base::ReadFileToString(p, &s);
}
string Property(const string &key) {
  return android::base::GetProperty(key, "");
}
bool CheckDevice() {
  string model = Property("ro.boot.em.model"),
         device = Property("ro.product.device");
  if (model.compare(0, 7, "SM-M115") && device != "m11q")
    return Fail("These tools are for Samsung m11q only");
  if (!Property("ro.boot.slot_suffix").empty())
    return Fail("A/B layout is not supported");
  return true;
}
bool Block(const string &name, string &path, uint64_t *size = nullptr) {
  char resolved[PATH_MAX];
  string link = "/dev/block/bootdevice/by-name/" + name;
  if (!realpath(link.c_str(), resolved))
    return Fail("Partition not found: " + name);
  path = resolved;
  struct stat s;
  if (path.compare(0, 11, "/dev/block/") || stat(path.c_str(), &s) ||
      !S_ISBLK(s.st_mode))
    return Fail("Invalid block target: " + name);
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  uint64_t bytes = 0;
  if (fd.n < 0 || ioctl(fd.n, BLKGETSIZE64, &bytes) || !bytes ||
      bytes > 128 * 1024 * 1024)
    return Fail("Invalid partition capacity");
  if ((name == "boot" || name == "recovery") && bytes != 67108864)
    return Fail("Expected a 64 MiB m11q boot/recovery partition");
  if (size)
    *size = bytes;
  return true;
}
bool VerificationDisabled() {
  string target;
  if (!Block("vbmeta", target))
    return false;
  Fd fd(open(target.c_str(), O_RDONLY | O_CLOEXEC));
  uint8_t h[256];
  if (fd.n < 0 || pread(fd.n, h, sizeof(h), 0) != sizeof(h) ||
      memcmp(h, "AVB0", 4))
    return Fail("Cannot verify current vbmeta flags");
  uint32_t flags = (uint32_t(h[120]) << 24) | (uint32_t(h[121]) << 16) |
                   (uint32_t(h[122]) << 8) | h[123];
  if (!(flags & 2))
    return Fail("AVB verification is enabled. This tool will not change "
                "vbmeta; use a ROM-supported image-patching method.");
  return true;
}
bool UnlockedForBootEdit() {
  const string state = android::base::GetProperty("ro.boot.vbmeta.device_state", "");
  const string verified = android::base::GetProperty("ro.boot.verifiedbootstate", "");
  const string flash_locked = android::base::GetProperty("ro.boot.flash.locked", "");
  gui_print("Bootloader state: device_state=%s, verifiedbootstate=%s, flash.locked=%s\n",
            state.empty() ? "missing" : state.c_str(),
            verified.empty() ? "missing" : verified.c_str(),
            flash_locked.empty() ? "missing" : flash_locked.c_str());
  if (!m11q::BootloaderUnlocked(state, verified, flash_locked))
    return Fail("Bootloader unlock state is not confirmed. No boot or vbmeta changes.");
  return true;
}
bool RomHashtreeDisabled() {
  string target;
  Bytes vbmeta;
  uint32_t flags;
  if (!Block("vbmeta", target) || !Read(target, vbmeta) || !m11q::AvbFlags(vbmeta,flags))
    return Fail("Cannot validate current top-level vbmeta.");
  if (!m11q::AvbHashtreeDisabled(flags))
    return Fail("ROM hashtree verification is enabled. Use AVB / DM-Verity before modifying stock restore files.");
  return true;
}
bool MkdirTree(const string &path) {
  if (path.empty() || path[0] != '/')
    return false;
  size_t pos = 1;
  while (true) {
    pos = path.find('/', pos);
    string part = pos == string::npos ? path : path.substr(0, pos);
    struct stat st;
    if (lstat(part.c_str(), &st)) {
      if (errno != ENOENT || mkdir(part.c_str(), 0700))
        return Fail("Cannot create " + part);
    } else if (!S_ISDIR(st.st_mode))
      return Fail("Unsafe directory: " + part);
    if (pos == string::npos)
      break;
    ++pos;
  }
  return true;
}
bool Storage(string &base) {
  if (!PartitionManager.Mount_Current_Storage(true))
    return Fail("Select writable storage first");
  char resolved[PATH_MAX];
  string path = DataManager::GetCurrentStoragePath();
  if (!realpath(path.c_str(), resolved) || string(resolved) == "/")
    return Fail("Invalid storage path");
  string serial = Property("ro.serialno");
  if (!SafeId(serial))
    serial = "m11q";
  base = string(resolved) + "/TWRP/Essentials/" + serial;
  return MkdirTree(base);
}
bool Session(const string &operation, string &dir) {
  string base;
  if (!Storage(base))
    return false;
  char date[32];
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &utc);
  string pattern = base + "/" + operation + "-" + date + "-XXXXXX";
  std::vector<char> tmp(pattern.begin(), pattern.end());
  tmp.push_back(0);
  if (!mkdtemp(tmp.data()))
    return Fail("Cannot create backup directory");
  dir = tmp.data();
  gui_print("Saved files: %s\n", dir.c_str());
  return true;
}
bool Space(const string &dir, size_t size) {
  struct statvfs v;
  if (statvfs(dir.c_str(), &v) ||
      uint64_t(v.f_bavail) * v.f_frsize < size + 16 * 1024 * 1024)
    return Fail("Not enough storage for verified backup");
  return true;
}
struct RomMount {
  TWPartition *part = nullptr;
  bool mounted = false, ro = true, changed = false, keep = false;
  bool Open(const string &path, bool writable = false) {
    part = PartitionManager.Find_Partition_By_Path(path);
    if (!part)
      return Fail("Partition unavailable: " + path);
    mounted = part->Is_Mounted();
    ro = part->Is_Read_Only();
    if (writable && ro) {
      if (mounted && !part->UnMount(true))
        return Fail("Cannot remount busy partition: " + path);
      part->Change_Mount_Read_Only(false);
      changed = true;
    }
    if (!part->Mount(true))
      return Fail("Cannot mount " + path);
    struct stat fs, block;
    if (stat(part->Get_Mount_Point().c_str(), &fs) ||
        stat(part->Actual_Block_Device.c_str(), &block) ||
        !S_ISBLK(block.st_mode) || fs.st_dev != block.st_rdev)
      return Fail("Mount does not refer to the ROM block device: " + path);
    if (writable && !part->Is_File_System_Writable())
      return Fail("ROM partition remains read-only: " + path);
    return true;
  }
  string Path() const { return part ? part->Get_Mount_Point() : ""; }
  ~RomMount() {
    if (!part || keep)
      return;
    if (!mounted || changed)
      part->UnMount(false);
    if (changed) {
      part->Change_Mount_Read_Only(ro);
      if (mounted)
        part->Mount(false);
    }
  }
};
string Fingerprint() {
  RomMount system;
  if (!system.Open(PartitionManager.Get_Android_Root_Path()))
    return "";
  for (const string &path :
       {system.Path() + "/system/build.prop", system.Path() + "/build.prop"}) {
    string text;
    if (!Text(path, text))
      continue;
    std::istringstream lines(text);
    string line;
    while (std::getline(lines, line)) {
      if (line.compare(0, 21, "ro.build.fingerprint=") == 0)
        return line.substr(21);
      if (line.compare(0, 28, "ro.system.build.fingerprint=") == 0)
        return line.substr(28);
    }
  }
  return "";
}
bool VerifiedSave(const string &path, const Bytes &b) {
  if (!Save(path, b))
    return false;
  Bytes back;
  if (!Read(path, back) || Hash(back) != Hash(b))
    return Fail("Backup readback failed");
  return Save(path + ".sha256", Hash(b) + "\n");
}
string KernelHash(const Bytes &b) {
  return Hash(b.data() + m11q::Le32(b, 36), m11q::Le32(b, 8));
}
bool BootSnapshot(const string &dir, const Bytes &b,
                  const string &fingerprint) {
  if (!Space(dir, b.size()) || !VerifiedSave(dir + "/boot.img", b))
    return false;
  return Save(dir + "/boot.meta", "quokka-boot-v1\n" + Hash(b) + "\n" +
                                      fingerprint + "\n" +
                                      std::to_string(m11q::Le32(b, 44)) + "\n" +
                                      KernelHash(b) + "\n");
}
bool ReadBoot(string &path, Bytes &b) {
  string error;
  return Block("boot", path) && Read(path, b) &&
         (m11q::ValidBoot(b, error) || Fail(error));
}
bool Flash(const string &path, const Bytes &original, const Bytes &updated,
           bool headerOnly = false, const string &label = "Boot") {
  if (updated.size() != original.size())
    return Fail("Image size changed unexpectedly");
  Fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
  if (fd.n < 0)
    return Fail("Cannot open " + label + " for writing");
  auto write = [&](const Bytes &data) {
    return headerOnly ? WriteBytes(fd.n, data.data() + 64, 512, 64) &&
                            WriteBytes(fd.n, data.data() + 608, 1024, 608)
                      : WriteBytes(fd.n, data.data(), data.size());
  };
  Bytes check;
  if (write(updated) && Read(path, check) && Hash(check) == Hash(updated)) {
    gui_print("%s write and full readback verified. Reboot manually.\n", label.c_str());
    return true;
  }
  gui_print("Write/readback failed; restoring saved %s bytes...\n", label.c_str());
  bool restored =
      write(original) && Read(path, check) && Hash(check) == Hash(original);
  gui_print("Rollback %s. Persistent backup is retained.\n",
            restored ? "verified" : "FAILED");
  return Fail("Boot change failed");
}
bool Selinux(const string &mode) {
  if (!VerificationDisabled())
    return false;
  string target, base, dir;
  Bytes original;
  if (!ReadBoot(target, original) || !Storage(base))
    return false;
  string state = base + "/selinux-original.state",
         before = m11q::Cmdline(original), fingerprint = Fingerprint();
  if (mode == "restore" && !Regular(state))
    return Fail("No original SELinux setting on selected storage");
  if (!Exists(state)) {
    string contents = KernelHash(original) + "\n" + before;
    if (!Save(state, contents) ||
        !Save(state + ".sha256", Hash(contents) + "\n"))
      return false;
  }
  string contents, digest;
  if (!Text(state, contents) || !Text(state + ".sha256", digest) ||
      Hash(contents) != m11q::Trim(digest) || contents.size() < 65 ||
      contents[64] != '\n' || contents.substr(0, 64) != KernelHash(original))
    return Fail("Original SELinux state is incomplete or kernel changed; do "
                "not reuse it");
  string first = contents.substr(65);
  Bytes updated = original;
  string error, next = mode == "restore" ? m11q::RestoreMode(before, first)
                                         : m11q::RequestedMode(before, mode);
  if (next == before) {
    gui_print("Boot already requests this setting. Android actual mode still "
              "needs getenforce after reboot.\n");
    return true;
  }
  if (!m11q::SetCmdline(updated, next, error))
    return Fail(error);
  if (!Session("selinux", dir) || !BootSnapshot(dir, original, fingerprint) ||
      !Save(dir + "/request.txt", mode + "\n"))
    return false;
  if (!Flash(target, original, updated, true))
    return false;
  if (mode == "restore") {
    unlink(state.c_str());
    unlink((state + ".sha256").c_str());
    sync();
  }
  gui_print("Android boot request: %s. Verify Android getenforce after reboot; "
            "recovery mode is separate.\n",
            mode.c_str());
  return true;
}
bool BackupBoot() {
  string path, dir;
  Bytes b;
  return ReadBoot(path, b) && Session("boot-backup", dir) &&
         BootSnapshot(dir, b, Fingerprint());
}
bool RestoreBoot(const string &selected) {
  if (!VerificationDisabled())
    return false;
  Bytes saved, current;
  string path, error, meta, digest;
  if (!Regular(selected) || !Text(selected + ".sha256", digest) ||
      !Read(selected, saved) || m11q::Trim(digest) != Hash(saved) ||
      !m11q::ValidBoot(saved, error))
    return Fail("Select a verified Essentials boot.img backup");
  string directory = selected.substr(0, selected.find_last_of('/'));
  if (!Text(directory + "/boot.meta", meta))
    return Fail("Missing boot backup metadata");
  std::istringstream lines(meta);
  string marker, sha, fingerprint, os, kernel;
  std::getline(lines, marker);
  std::getline(lines, sha);
  std::getline(lines, fingerprint);
  std::getline(lines, os);
  std::getline(lines, kernel);
  if (marker != "quokka-boot-v1" || sha != Hash(saved) ||
      os != std::to_string(m11q::Le32(saved, 44)) ||
      kernel != KernelHash(saved))
    return Fail("Invalid boot backup metadata");
  if (fingerprint.empty() || Fingerprint() != fingerprint)
    return Fail("ROM fingerprint does not match this backup");
  if (!ReadBoot(path, current) || current.size() != saved.size())
    return false;
  string dir;
  if (!Session("before-restore", dir) ||
      !BootSnapshot(dir, current, fingerprint))
    return false;
  return Flash(path, current, saved);
}
bool BackupEfs() {
  string target, dir;
  Bytes image;
  if (!Block("sec_efs", target) || !Read(target, image) ||
      !Session("efs-backup", dir) || !Space(dir, image.size()))
    return false;
  if (!VerifiedSave(dir + "/sec_efs.img", image))
    return false;
  gui_print("sec_efs.img backed up. Restore with Sec EFS Image; "
            "the separate efs partition is not included.\n");
  return true;
}
bool AvbStatus() {
  string target;
  Bytes current;
  uint32_t flags;
  if (!Block("vbmeta",target) || !Read(target,current) || !m11q::AvbFlags(current,flags))
    return Fail("Cannot validate current top-level vbmeta.");
  gui_print("AVB flags: %u. Verification: %s. Hashtree: %s.\n", flags,
            flags & 2 ? "disabled" : "enabled",
            m11q::AvbHashtreeDisabled(flags) ? "disabled" : "enabled");
  gui_print("Bootloader state: %s / %s. FBE settings are separate.\n",
            android::base::GetProperty("ro.boot.vbmeta.device_state", "unknown").c_str(),
            android::base::GetProperty("ro.boot.verifiedbootstate", "unknown").c_str());
  return true;
}
bool PrepareAvb() {
  if (!UnlockedForBootEdit()) return false;
  string target, boot_target, dir;
  Bytes original, changed, boot;
  uint32_t flags;
  if (!Block("vbmeta", target) || !Read(target,original) ||
      !m11q::AvbFlags(original,flags) || !m11q::PrepareAvb(original,changed))
    return Fail("Unsupported top-level vbmeta. No changes.");
  if (flags == 3) {
    gui_print("AVB verification and hashtree are already disabled. No changes.\n");
    return true;
  }
  if (!ReadBoot(boot_target,boot) || !Session("avb-prepare",dir) ||
      !Space(dir,boot.size()+original.size()*2) ||
      !VerifiedSave(dir+"/vbmeta-original.img",original) ||
      !VerifiedSave(dir+"/vbmeta-disabled.img",changed) ||
      !BootSnapshot(dir,boot,Fingerprint())) return false;
  string metadata = "quokka-avb-v1\noriginal="+Hash(original)+"\nprepared="+Hash(changed)+
      "\nflags-before="+std::to_string(flags)+"\nflags-after=3\n";
  if (!Save(dir+"/avb.meta",metadata) || !Save(dir+"/RESTORE.txt",
      "Keep these images with the matching firmware. After changing boot or ROM files, "
      "do not re-enable verification by restoring vbmeta alone. Restore the complete "
      "matching stock firmware when returning to verified stock.\n")) return false;
  gui_print("Preparing the current vbmeta; only its verification flags change.\n"
            "The original signature will no longer match; unlocked bootloader is required.\n"
            "Data encryption and partition layout are unchanged.\n");
  if (!Flash(target,original,changed,false,"VBMeta")) return false;
  gui_print("AVB prepared. Keep TWRP may now check and disable detected restore files.\n"
            "Reboot manually; stock boot compatibility still requires testing.\n");
  return true;
}
bool PreventStockRestore() {
  if (!UnlockedForBootEdit())
    return false;
  string dir;
  if (!Session("stock-restore", dir))
    return false;
  std::vector<std::unique_ptr<RomMount>> mounts;
  std::vector<string> targets;
  for (const string &point :
       {PartitionManager.Get_Android_Root_Path(), string("/vendor")}) {
    auto mount = std::unique_ptr<RomMount>(new RomMount);
    if (!mount->Open(point))
      return false;
    string root = mount->Path();
    std::vector<string> paths = {root + "/recovery-from-boot.p",
                                 root + "/system/recovery-from-boot.p"};
    for (const string &script :
         {root + "/bin/install-recovery.sh", root + "/etc/install-recovery.sh",
          root + "/system/bin/install-recovery.sh",
          root + "/system/etc/install-recovery.sh"}) {
      string text;
      if (Text(script, text) && text.find("recovery") != string::npos &&
          (text.find("applypatch") != string::npos ||
           text.find("flash_image") != string::npos))
        paths.push_back(script);
    }
    for (const auto &p : paths)
      if (Regular(p)) {
        char resolved[PATH_MAX], mountPath[PATH_MAX];
        struct stat file, fs;
        if (!realpath(p.c_str(), resolved) ||
            !realpath(root.c_str(), mountPath) || stat(p.c_str(), &file) ||
            stat(root.c_str(), &fs) || file.st_dev != fs.st_dev ||
            string(resolved).compare(0, strlen(mountPath) + 1,
                                     string(mountPath) + "/"))
          return Fail("Restore file is outside its ROM filesystem");
        if (std::find(targets.begin(), targets.end(), string(resolved)) ==
            targets.end())
          targets.push_back(resolved);
      }
    mounts.push_back(std::move(mount));
  }
  if (targets.empty()) {
    gui_print("No supported stock recovery restore files detected. No changes; "
              "KG status was not modified.\n");
    return true;
  }
  if (!RomHashtreeDisabled()) return false;
  // Complete backups before the first ROM modification.
  string manifest;
  for (size_t i = 0; i < targets.size(); ++i) {
    Bytes data;
    string p = targets[i];
    if (Exists(p + ".quokka-disabled"))
      return Fail("Existing disabled backup: " + p);
    if (!Read(p, data, 32 * 1024 * 1024) || !Space(dir, data.size()) ||
        !VerifiedSave(dir + "/file-" + std::to_string(i), data))
      return false;
    manifest += p + " " + Hash(data) + "\n";
  }
  if (!Save(dir + "/files.txt", manifest))
    return false;
  // Upgrade only mounts containing files that will actually be changed.
  for (auto &mount : mounts) {
    bool needed = false;
    for (auto &p : targets)
      if (p.compare(0, mount->Path().size() + 1, mount->Path() + "/") == 0)
        needed = true;
    if (needed) {
      string point = mount->Path();
      mount.reset();
      mount.reset(new RomMount);
      if (!mount->Open(point, true))
        return false;
    }
  }
  size_t done = 0;
  for (; done < targets.size(); ++done) {
    string p = targets[done];
    if (rename(p.c_str(), (p + ".quokka-disabled").c_str()) || Exists(p) ||
        !Regular(p + ".quokka-disabled"))
      break;
    gui_print("Disabled: %s\n", p.c_str());
  }
  if (done != targets.size()) {
    for (size_t i = 0; i < done; ++i)
      rename((targets[i] + ".quokka-disabled").c_str(), targets[i].c_str());
    sync();
    return Fail("Could not disable all restore files; rollback attempted, "
                "backups retained");
  }
  sync();
  gui_print("Stock restore files disabled. vaultkeeperd unchanged; no claim "
            "about KG state.\n");
  return true;
}
bool MountRw() {
  bool ok = true;
  int count = 0;
  for (const string &point :
       {PartitionManager.Get_Android_Root_Path(), string("/vendor"),
        string("/product"), string("/odm")}) {
    if (!PartitionManager.Find_Partition_By_Path(point)) {
      gui_print("Not present: %s\n", point.c_str());
      continue;
    }
    RomMount mount;
    if (!mount.Open(point, true)) {
      ok = false;
      continue;
    }
    mount.keep = true;
    ++count;
    gui_print("Mounted RW for this recovery session: %s\n",
              mount.Path().c_str());
  }
  return count > 0 && ok;
}
bool DataReady() {
  TWPartition *data = PartitionManager.Find_Partition_By_Path("/data");
  if (!data ||
      (DataManager::GetIntValue(TW_IS_ENCRYPTED) &&
       !DataManager::GetIntValue(TW_IS_DECRYPTED)) ||
      !data->Mount(true) || !data->Is_File_System_Writable())
    return Fail("Decrypt and mount Data before managing modules");
  // Newer KernelSU initrc injection requires matching ksud handling.
  for (const string &p :
       {"/metadata/ksu/modules.rc", "/metadata/watchdog/ksu/modules.rc"})
    if (Exists(p))
      return Fail("KernelSU initrc injection detected; use the matching "
                  "KernelSU rescue method");
  return true;
}
using Module = m11q::ModuleState;
bool ModuleAt(const string &id, Module &m) {
  string path = "/data/adb/modules/" + id, prop;
  if (!SafeId(id) || !Directory(path) || !Text(path + "/module.prop", prop))
    return Fail("Not a supported module: " + id);
  if (Exists(path + "/disable") && !Regular(path + "/disable"))
    return Fail("Invalid disable marker: " + id);
  m = {id, Hash(prop), Exists(path + "/disable")};
  return true;
}
bool Modules(std::vector<Module> &list) {
  if (!DataReady())
    return false;
  if (!Directory("/data/adb/modules")) {
    gui_print("No module directory found.\n");
    return true;
  }
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/data/adb/modules"),
                                                closedir);
  if (!dir)
    return Fail("Cannot read module directory");
  struct dirent *entry;
  while ((entry = readdir(dir.get()))) {
    string id = entry->d_name;
    if (id == "." || id == ".." || !Directory("/data/adb/modules/" + id))
      continue;
    Module m;
    if (!ModuleAt(id, m))
      return false;
    list.push_back(m);
  }
  std::sort(list.begin(), list.end(),
            [](const Module &a, const Module &b) { return a.id < b.id; });
  for (auto &m : list)
    gui_print("%s: %s\n", m.id.c_str(), m.disabled ? "disabled" : "enabled");
  return true;
}
bool ModuleDisable(const string &selected, bool all) {
  std::vector<Module> list;
  if (all) {
    if (!Modules(list))
      return false;
  } else {
    if (!DataReady())
      return false;
    const string prefix = "/data/adb/modules/";
    if (selected.compare(0, prefix.size(), prefix) ||
        selected.size() < prefix.size() + 13 ||
        selected.substr(selected.size() - 12) != "/module.prop")
      return Fail("Select module.prop inside one installed module");
    Module m;
    string id =
        selected.substr(prefix.size(), selected.size() - prefix.size() - 12);
    if (!ModuleAt(id, m))
      return false;
    list.push_back(m);
  }
  if (list.empty()) {
    gui_print("No modules to disable.\n");
    return true;
  }
  string dir, state = "quokka-modules-v1\n";
  for (auto &m : list)
    state += m.id + " " + (m.disabled ? "1" : "0") + " " + m.propHash + "\n";
  if (!Session("modules", dir) || !Save(dir + "/modules.state", state) ||
      !Save(dir + "/modules.state.sha256", Hash(state) + "\n"))
    return false;
  for (auto &m : list)
    if (!m.disabled &&
        !Save("/data/adb/modules/" + m.id + "/disable", string("")))
      return false;
  sync();
  gui_print("Modules disabled; reboot manually. Restore using the saved "
            "modules.state.\n");
  return true;
}
bool ModuleRestore(const string &selected) {
  if (!DataReady())
    return false;
  string state, digest;
  if (!Text(selected, state) || !Text(selected + ".sha256", digest) ||
      Hash(state) != m11q::Trim(digest))
    return Fail("Select a verified modules.state backup");
  std::vector<Module> list;
  if (!m11q::ParseModuleState(state, list))
    return Fail("Invalid or empty module state");
  for (auto &m : list) {
    Module current;
    if (!ModuleAt(m.id, current) || current.propHash != m.propHash)
      return Fail("Module changed since backup: " + m.id);
  }
  // All identities checked before changing a marker.
  for (auto &m : list) {
    string markerPath = "/data/adb/modules/" + m.id + "/disable";
    if (m.disabled && !Exists(markerPath)) {
      if (!Save(markerPath, string("")))
        return false;
    } else if (!m.disabled && Exists(markerPath) && unlink(markerPath.c_str()))
      return Fail("Cannot restore module: " + m.id);
  }
  sync();
  gui_print("Previous module states restored.\n");
  return true;
}
bool Diagnostics() {
  string dir;
  if (!Session("diagnostics", dir))
    return false;
  string summary =
      "Recovery: " + Property("ro.twrp.version") + "\nRecovery SELinux: ";
  string enforcing;
  if (android::base::ReadFileToString("/sys/fs/selinux/enforce", &enforcing))
    summary += m11q::Trim(enforcing) == "1" ? "Enforcing" : "Permissive";
  else
    summary += "unknown";
  summary += "\nAndroid actual SELinux: not measured in recovery\n";
  for (const string &key :
       {"ro.crypto.state", "ro.crypto.type", "ro.boot.verifiedbootstate",
        "init.svc.qseecomd", "init.svc.keystore2",
        "init.svc.vendor.keymaster-4-0", "init.svc.vendor.gatekeeper-1-0"})
    summary += key + "=" + Property(key) + "\n";
  string target;
  Bytes boot;
  string error;
  if (Block("boot", target) && Read(target, boot) &&
      m11q::ValidBoot(boot, error))
    summary += "Boot cmdline: " + m11q::Cmdline(boot) +
               "\nBoot SHA256: " + Hash(boot) + "\n";
  if (!Save(dir + "/summary.txt", summary))
    return false;
  bool ok = true;
  for (const string &p :
       {"/proc/mounts", "/proc/cmdline", "/tmp/recovery.log"}) {
    string text;
    if (android::base::ReadFileToString(p, &text) &&
        !Save(dir + "/" + p.substr(p.find_last_of('/') + 1), text))
      ok = false;
  }
  string result;
  for (const string &tool : {"dmesg", "logcat -d -b all"}) {
    result.clear();
    int status = TWFunc::Exec_Cmd(tool, result, true);
    string name = tool == "dmesg" ? "kernel.log" : "logcat.txt";
    if (!Save(dir + "/" + name, result))
      ok = false;
    if (status)
      gui_print("Optional capture unavailable: %s\n", tool.c_str());
  }
  if (Directory("/tmp/tombstones")) {
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir("/tmp/tombstones"),
                                                closedir);
    struct dirent *e;
    if (d)
      while ((e = readdir(d.get()))) {
        string name = e->d_name;
        if (!SafeId(name) || !Regular("/tmp/tombstones/" + name))
          continue;
        Bytes b;
        if (!Read("/tmp/tombstones/" + name, b, 16 * 1024 * 1024) ||
            !Save(dir + "/" + name, b))
          ok = false;
      }
  }
  return ok;
}
bool MagiskInstall() {
  if (!UnlockedForBootEdit())
    return false;
  Bytes apk, boot;
  string target, error, dir, fingerprint = Fingerprint();
  Bytes wrapper, script;
  if (!Read("/addon/magisk-30.7.zip", apk, 32 * 1024 * 1024) ||
      Hash(apk) != "e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5" ||
      !Read("/addon/quokka-magisk.zip", wrapper, 1024 * 1024) ||
      Hash(wrapper) != "df72610d10110db58ca3b70dd563f96b56ab42569f24734c57afc29cfdebd065" ||
      !Read("/addon/quokka-magisk-updater.sh", script, 1024 * 1024) ||
      Hash(script) != "84157455f4099214c70a6fc1044e45926c2f8e6d50e77c1ac88b1e251ef70643")
    return Fail("Bundled Magisk checksum mismatch");
  if (!ReadBoot(target, boot) || !m11q::Le32(boot, 16))
    return Fail(
        "No supported boot ramdisk. Use the Magisk App image patching method.");
  if (!Session("magisk-30.7", dir) || !BootSnapshot(dir, boot, fingerprint))
    return false;
  gui_print("Installing Magisk 30.7 into boot. Encryption/verity are retained; "
            "recovery/vbmeta are not installer targets.\n");
  struct Env {
    string key, value;
    bool present;
  };
  std::vector<Env> vars;
  for (auto entry : std::vector<std::pair<string, string>>{
           {"QUOKKA_ESSENTIALS_INSTALL", "1"}}) {
    const char *old = getenv(entry.first.c_str());
    vars.push_back({entry.first, old ? old : "", old != nullptr});
    setenv(entry.first.c_str(), entry.second.c_str(), 1);
  }
  int wipe = 0,
      status = TWinstall_zip("/addon/quokka-magisk.zip", &wipe, false);
  for (auto &v : vars) {
    if (v.present)
      setenv(v.key.c_str(), v.value.c_str(), 1);
    else
      unsetenv(v.key.c_str());
  }
  if (status) {
    gui_print("Installer failed; original boot backup: %s/boot.img\n",
              dir.c_str());
    return Fail("Magisk installation failed; data changes may require cleanup");
  }
  Bytes changed;
  if (!Read(target, changed) || !m11q::ValidBoot(changed, error) ||
      Hash(changed) == Hash(boot))
    return Fail(
        "Installer reported success but no valid boot change was verified");
  if (!Save(dir + "/installed-boot.sha256", Hash(changed) + "\n"))
    return false;
  gui_print("Boot readback recorded. Reboot manually; confirm Magisk root in "
            "Android.\n");
  return true;
}
} // namespace

int M11qEssentials(const string &operation) {
  if (!CheckDevice())
    return 1;
  string selected = DataManager::GetStrValue("tw_m11q_file");
  bool ok = false;
  if (operation == "selinux-enforcing")
    ok = Selinux("enforcing");
  else if (operation == "selinux-permissive")
    ok = Selinux("permissive");
  else if (operation == "selinux-restore")
    ok = Selinux("restore");
  else if (operation == "boot-backup")
    ok = BackupBoot();
  else if (operation == "boot-restore")
    ok = RestoreBoot(selected);
  else if (operation == "efs-backup")
    ok = BackupEfs();
  else if (operation == "stock-restore")
    ok = PreventStockRestore();
  else if (operation == "avb-status")
    ok = AvbStatus();
  else if (operation == "avb-prepare")
    ok = PrepareAvb();
  else if (operation == "mount-rw")
    ok = MountRw();
  else if (operation == "modules-list") {
    std::vector<Module> list;
    ok = Modules(list);
  } else if (operation == "module-disable")
    ok = ModuleDisable(selected, false);
  else if (operation == "modules-disable")
    ok = ModuleDisable("", true);
  else if (operation == "modules-restore")
    ok = ModuleRestore(selected);
  else if (operation == "diagnostics")
    ok = Diagnostics();
  else if (operation == "magisk-install")
    ok = MagiskInstall();
  else
    ok = Fail("Unknown Essentials operation");
  return ok ? 0 : 1;
}
