/**
 * This file is part of the CernVM File System.
 */

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <string>

#include "../../cvmfs/cache.h"
#include "../../cvmfs/compression.h"
#include "../../cvmfs/hash.h"
#include "../../cvmfs/platform.h"
#include "../../cvmfs/quota.h"
#include "testutil.h"

using namespace std;  // NOLINT

namespace cache {

class T_CacheManager : public ::testing::Test {
 protected:
  virtual void SetUp() {
    used_fds_ = GetNoUsedFds();

    tmp_path_ = CreateTempDir("/tmp/cvmfs_test");
    cache_mgr_ = PosixCacheManager::Create(tmp_path_, false);
    ASSERT_TRUE(cache_mgr_ != NULL);
    alien_cache_mgr_ = PosixCacheManager::Create(tmp_path_, true);
    ASSERT_TRUE(alien_cache_mgr_ != NULL);

    ASSERT_TRUE(cache_mgr_->CommitFromMem(hash_null_, NULL, 0, "null"));
    unsigned char buf = 'A';
    hash_one_.digest[0] = 1;
    ASSERT_TRUE(cache_mgr_->CommitFromMem(hash_one_, &buf, 1, "one"));
  }

  virtual void TearDown() {
    delete cache_mgr_;
    delete alien_cache_mgr_;

    // Empty transaction tmp path
    platform_stat64 info;
    string cache_path = tmp_path_ + "/txn";
    EXPECT_EQ(0, platform_stat(cache_path.c_str(), &info));
    EXPECT_EQ(2U, info.st_nlink);  // empty directory

    if (tmp_path_ != "")
      RemoveTree(tmp_path_);
    EXPECT_EQ(used_fds_, GetNoUsedFds());
  }

  struct TearDownCb {
    TearDownCb(PosixCacheManager *mgr) : mgr(mgr), finished(false) { }
    PosixCacheManager *mgr;
    bool finished;
  };

  static void *MainTearDown(void *data) {
    TearDownCb *cb = reinterpret_cast<TearDownCb *>(data);
    cb->mgr->TearDown2ReadOnly();
    cb->finished = true;
    return NULL;
  }

  bool TearDownTimedOut(PosixCacheManager *mgr) {
    TearDownCb cb(mgr);
    pthread_t thread_teardown;
    int retval = pthread_create(&thread_teardown, NULL, MainTearDown, &cb);
    assert(retval == 0);
    unsigned sum_ms = 0;
    while (!cb.finished) {
      SafeSleepMs(50);
      sum_ms += 50;
      if (sum_ms > 250)
        break;
    }
    if (sum_ms > 250) {
      retval = pthread_cancel(thread_teardown);
      assert(retval == 0);
      return true;
    }
    pthread_join(thread_teardown, 0);
    return false;
  }

 protected:
  PosixCacheManager *cache_mgr_;
  PosixCacheManager *alien_cache_mgr_;
  string tmp_path_;
  shash::Any hash_null_;
  shash::Any hash_one_;
  unsigned used_fds_;
};


/**
 * Used to check if quota commands are correctly sent to the QuotaManager.  Just
 * records the last command sent to the quota manager.
 */
class TestQuotaManager : public QuotaManager {
 public:
  enum Command {
    kCmdUnknown,
    kCmdInsert,
    kCmdInsertVolatile,
    kCmdPin,
    kCmdUnpin,
    kCmdTouch,
    kCmdRemove,
    kCmdCleanup,
  };

  struct LastCommand {
    LastCommand()
      : cmd(kCmdUnknown)
      , size(0)
      , is_catalog(false)
    { }

    Command cmd;
    shash::Any hash;
    uint64_t size;
    std::string description;
    bool is_catalog;
  };

  virtual bool IsEnforcing() { return true; }

  virtual void Insert(const shash::Any &hash, const uint64_t size,
                      const std::string &description)
  {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdInsert;
    last_cmd.hash = hash;
    last_cmd.size = size;
    last_cmd.description = description;
  }

  virtual void InsertVolatile(const shash::Any &hash, const uint64_t size,
                              const std::string &description)
  {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdInsertVolatile;
    last_cmd.hash = hash;
    last_cmd.size = size;
    last_cmd.description = description;
  }
  virtual bool Pin(const shash::Any &hash, const uint64_t size,
                   const std::string &description, const bool is_catalog)
  {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdPin;
    last_cmd.hash = hash;
    last_cmd.size = size;
    last_cmd.description = description;
    last_cmd.is_catalog = is_catalog;
    return true;
  }
  virtual void Unpin(const shash::Any &hash) {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdUnpin;
    last_cmd.hash = hash;
  }
  virtual void Touch(const shash::Any &hash) {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdTouch;
    last_cmd.hash = hash;
  }
  virtual void Remove(const shash::Any &file) {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdRemove;
    last_cmd.hash = file;
  }
  virtual bool Cleanup(const uint64_t leave_size) {
    last_cmd = LastCommand();
    last_cmd.cmd = kCmdUnpin;
    last_cmd.size = leave_size;
    return true;
  }

  virtual void RegisterBackChannel(int back_channel[2],
                                   const std::string &channel_id) { }
  virtual void UnregisterBackChannel(int back_channel[2],
                                     const std::string &channel_id) { }

  virtual std::vector<std::string> List() { return std::vector<std::string>(); }
  virtual std::vector<std::string> ListPinned() {
    return std::vector<std::string>();
  }
  virtual std::vector<std::string> ListCatalogs() {
    return std::vector<std::string>();
  }
  virtual uint64_t GetMaxFileSize() { return 0; }
  virtual uint64_t GetCapacity() { return 0; }
  virtual uint64_t GetSize() { return 0; }
  virtual uint64_t GetSizePinned() { return 0; }

  virtual void Spawn() { }
  virtual pid_t GetPid() { return getpid(); }
  virtual uint32_t GetProtocolRevision() { return 0; }

  LastCommand last_cmd;
};


/**
 * Does mostly nothing, read and write attempts fail. Uses the TestQuotaManager.
 */
class TestCacheManager : public CacheManager {
 public:
  TestCacheManager() {
    delete quota_mgr_;
    quota_mgr_ = new TestQuotaManager();
  }
  virtual int Open(const shash::Any &id) { return open("/dev/null", O_RDONLY); }
  virtual int64_t GetSize(int fd) { return 1; }
  virtual int Close(int fd) { return close(fd); }
  virtual int64_t Pread(int fd, void *buf, uint64_t size, uint64_t offset) {
    return -EIO;
  }
  virtual uint16_t SizeOfTxn() { return sizeof(int); }
  virtual int StartTxn(const shash::Any &id, void *txn) {
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    *static_cast<int *>(txn) = fd;
    return 0;
  }
  virtual void CtrlTxn(
    const std::string &description, const int flags, void *txn) { }
  virtual int64_t Write(const void *buf, uint64_t sz, void *txn) {
    return -EIO;
  }
  virtual int Reset(void *txn) { return 0; }
  virtual int AbortTxn(void *txn, const std::string &dump_path = "") {
    return close(*static_cast<int *>(txn));
  }
  virtual int OpenFromTxn(void *txn) { return open("/dev/null", O_RDONLY); }
  virtual int CommitTxn(void *txn) {
    return 0;
  }
};


TEST_F(T_CacheManager, CommitFromMem) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  unsigned char buf = '1';
  EXPECT_TRUE(cache_mgr_->CommitFromMem(rnd_hash, &buf, 1, "1"));
  unsigned char *retrieve_buf;
  uint64_t retrieve_size;
  EXPECT_TRUE(cache_mgr_->Open2Mem(rnd_hash, &retrieve_buf, &retrieve_size));
  EXPECT_EQ(1U, retrieve_size);
  EXPECT_EQ('1', retrieve_buf[0]);
  free(retrieve_buf);

  TestCacheManager faulty_cache;
  EXPECT_FALSE(faulty_cache.CommitFromMem(rnd_hash, &buf, 1, "1"));

  string final_dir = tmp_path_ + "/" + rnd_hash.MakePath();
  EXPECT_EQ(0, unlink((tmp_path_ + "/" + hash_null_.MakePath()).c_str()));
  EXPECT_EQ(0, unlink((tmp_path_ + "/" + hash_one_.MakePath()).c_str()));
  EXPECT_EQ(0, unlink(final_dir.c_str()));
  EXPECT_EQ(0, rmdir(GetParentPath(final_dir).c_str()));
  EXPECT_FALSE(cache_mgr_->CommitFromMem(rnd_hash, &buf, 1, "1"));
}


TEST_F(T_CacheManager, Open2Mem) {
  unsigned char *retrieve_buf;
  uint64_t retrieve_size;

  EXPECT_FALSE(cache_mgr_->Open2Mem(shash::Any(shash::kMd5),
    &retrieve_buf, &retrieve_size));

  EXPECT_TRUE(cache_mgr_->Open2Mem(hash_null_, &retrieve_buf, &retrieve_size));
  EXPECT_EQ(0U, retrieve_size);
  EXPECT_EQ(NULL, retrieve_buf);

  EXPECT_TRUE(cache_mgr_->Open2Mem(hash_one_, &retrieve_buf, &retrieve_size));
  EXPECT_EQ(1U, retrieve_size);
  EXPECT_EQ('A', retrieve_buf[0]);

  TestCacheManager faulty_cache;
  EXPECT_FALSE(faulty_cache.Open2Mem(hash_one_, &retrieve_buf, &retrieve_size));
  EXPECT_EQ(0U, retrieve_size);
  EXPECT_EQ(NULL, retrieve_buf);
}


//------------------------------------------------------------------------------


TEST_F(T_CacheManager, AbortTxn) {
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);

  EXPECT_GE(cache_mgr_->StartTxn(hash_null_, txn), 0);
  EXPECT_EQ(0, cache_mgr_->AbortTxn(txn));

  EXPECT_GE(cache_mgr_->StartTxn(hash_one_, txn), 0);
  EXPECT_TRUE(RemoveTree(tmp_path_ + "/txn"));
  EXPECT_EQ(0, mkdir((tmp_path_ + "/txn").c_str(), 0700));
  EXPECT_EQ(-ENOENT, cache_mgr_->AbortTxn(txn));

  string dump_path = tmp_path_ + "/dump";
  EXPECT_GE(cache_mgr_->StartTxn(hash_null_, txn), 0);
  EXPECT_EQ(int64_t(dump_path.length()),
            cache_mgr_->Write(dump_path.data(), dump_path.length(), txn));
  EXPECT_EQ(0, cache_mgr_->AbortTxn(txn, dump_path));
  unsigned char *buf;
  unsigned buf_size;
  EXPECT_TRUE(CopyPath2Mem(dump_path, &buf, &buf_size));
  EXPECT_EQ(buf_size, dump_path.length());
  EXPECT_EQ(dump_path, string(reinterpret_cast<char *>(buf), buf_size));
}


TEST_F(T_CacheManager, Close) {
  int fd = cache_mgr_->Open(hash_null_);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->Close(fd));
  EXPECT_EQ(-EBADF, cache_mgr_->Close(fd));
}


TEST_F(T_CacheManager, CommitTxn) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);
  int fd;

  ASSERT_EQ(-ENOENT, cache_mgr_->Open(rnd_hash));

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));
  fd = cache_mgr_->Open(rnd_hash);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->GetSize(fd));
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  // Test flushing
  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  unsigned char buf = 'A';
  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));
  fd = cache_mgr_->Open(rnd_hash);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(1, cache_mgr_->GetSize(fd));
  EXPECT_EQ(1, cache_mgr_->Pread(fd, &buf, 1, 0));
  EXPECT_EQ('A', buf);
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  // Test alien cache file mode
  platform_stat64 info;
  string cache_path = tmp_path_ + "/" + rnd_hash.MakePath();
  EXPECT_EQ(0, platform_stat(cache_path.c_str(), &info));
  EXPECT_EQ(0600U, info.st_mode & 0x03FF);
  EXPECT_GE(alien_cache_mgr_->StartTxn(rnd_hash, txn), 0);
  EXPECT_EQ(0, alien_cache_mgr_->CommitTxn(txn));
  EXPECT_EQ(0, platform_stat(cache_path.c_str(), &info));
  EXPECT_EQ(0660U, info.st_mode & 0x03FF);

  // Test quota notifications
  delete cache_mgr_->quota_mgr_;
  cache_mgr_->quota_mgr_ = new TestQuotaManager();
  TestQuotaManager *quota_mgr = reinterpret_cast<TestQuotaManager *>(
    cache_mgr_->quota_mgr());

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));
  EXPECT_EQ(TestQuotaManager::kCmdInsert, quota_mgr->last_cmd.cmd);
  EXPECT_EQ(rnd_hash, quota_mgr->last_cmd.hash);
  EXPECT_EQ(1U, quota_mgr->last_cmd.size);

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  cache_mgr_->CtrlTxn("desc", CacheManager::kFlagVolatile, txn);
  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));
  EXPECT_EQ(TestQuotaManager::kCmdInsertVolatile, quota_mgr->last_cmd.cmd);
  EXPECT_EQ(rnd_hash, quota_mgr->last_cmd.hash);
  EXPECT_EQ(1U, quota_mgr->last_cmd.size);
  EXPECT_EQ("desc", quota_mgr->last_cmd.description);
}


TEST_F(T_CacheManager, CommitTxnRenameFail) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);

  ASSERT_EQ(-ENOENT, cache_mgr_->Open(rnd_hash));

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  string final_dir = GetParentPath(tmp_path_ + "/" + rnd_hash.MakePath());
  EXPECT_EQ(0, unlink((tmp_path_ + "/" + hash_null_.MakePath()).c_str()));
  EXPECT_EQ(0, unlink((tmp_path_ + "/" + hash_one_.MakePath()).c_str()));
  EXPECT_EQ(0, rmdir(final_dir.c_str()));
  EXPECT_EQ(-ENOENT, cache_mgr_->CommitTxn(txn));
}


TEST_F(T_CacheManager, CommitTxnFlushFail) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);

  ASSERT_EQ(-ENOENT, cache_mgr_->Open(rnd_hash));

  int fd = cache_mgr_->StartTxn(rnd_hash, txn);
  EXPECT_GE(fd, 0);
  unsigned char buf = 'A';
  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  EXPECT_EQ(0, close(fd));
  EXPECT_EQ(-EBADF, cache_mgr_->CommitTxn(txn));
}


TEST_F(T_CacheManager, Create) {
  string path = tmp_path_ + "/test";
  MkdirDeep(path, 0700);
  EXPECT_EQ(NULL, PosixCacheManager::Create("/dev/null", false));
  EXPECT_EQ(NULL, PosixCacheManager::Create("/dev/null", true));

  PosixCacheManager *mgr = PosixCacheManager::Create(path, false);
  EXPECT_TRUE(mgr != NULL);
  EXPECT_TRUE(DirectoryExists(path + "/ff"));
  platform_stat64 info;
  EXPECT_EQ(0, platform_stat((path + "/ff").c_str(), &info));
  EXPECT_EQ(0700U, info.st_mode & 0x03FF);
  delete mgr;

  mode_t mask_save = umask(000);
  string path2 = path + "2";
  MkdirDeep(path2, 0700);
  mgr = PosixCacheManager::Create(path2, true);
  EXPECT_TRUE(mgr != NULL);
  EXPECT_TRUE(DirectoryExists(path2 + "/ff"));
  EXPECT_EQ(0, platform_stat((path2 + "/ff").c_str(), &info));
  EXPECT_EQ(0770U, info.st_mode & 0x03FF);
  delete mgr;
  umask(mask_save);

  CopyPath2Path(tmp_path_ + "/" + hash_null_.MakePath(),
                path + "/cvmfscatalog.cache");
  EXPECT_EQ(NULL, PosixCacheManager::Create(path, false));
}


TEST_F(T_CacheManager, GetSize) {
  int fd = cache_mgr_->Open(hash_null_);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->GetSize(fd));
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  fd = cache_mgr_->Open(hash_one_);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(1, cache_mgr_->GetSize(fd));
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  EXPECT_EQ(-EBADF, cache_mgr_->GetSize(fd));
}


TEST_F(T_CacheManager, Open) {
  delete cache_mgr_->quota_mgr_;
  cache_mgr_->quota_mgr_ = new TestQuotaManager();
  TestQuotaManager *quota_mgr = reinterpret_cast<TestQuotaManager *>(
    cache_mgr_->quota_mgr());

  shash::Any rnd_hash;
  rnd_hash.Randomize();
  EXPECT_EQ(-ENOENT, cache_mgr_->Open(rnd_hash));
  EXPECT_EQ(TestQuotaManager::kCmdUnknown, quota_mgr->last_cmd.cmd);

  int fd = cache_mgr_->Open(hash_null_);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->Close(fd));
  EXPECT_EQ(TestQuotaManager::kCmdTouch, quota_mgr->last_cmd.cmd);
  EXPECT_EQ(hash_null_, quota_mgr->last_cmd.hash);
}


TEST_F(T_CacheManager, OpenFromTxn) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);

  ASSERT_EQ(-ENOENT, cache_mgr_->Open(rnd_hash));

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  unsigned char buf = 'A';
  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  int fd = cache_mgr_->OpenFromTxn(txn);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(1U, cache_mgr_->GetSize(fd));
  EXPECT_EQ(1, cache_mgr_->Pread(fd, &buf, 1, 0));
  EXPECT_EQ('A', buf);
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  PosixCacheManager::Transaction *transaction =
    reinterpret_cast<PosixCacheManager::Transaction *>(txn);
  EXPECT_EQ(0, unlink(transaction->tmp_path.c_str()));
  EXPECT_EQ(-ENOENT, cache_mgr_->OpenFromTxn(txn));

  EXPECT_EQ(1U, cache_mgr_->Write(&buf, 1, txn));
  EXPECT_EQ(0, close(transaction->fd));
  EXPECT_EQ(-EBADF, cache_mgr_->OpenFromTxn(txn));

  cache_mgr_->AbortTxn(txn);
}


TEST_F(T_CacheManager, Pread) {
  char buf[1024];
  int fd = cache_mgr_->Open(hash_one_);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(1U, cache_mgr_->Pread(fd, &buf, 1024, 0));
  EXPECT_EQ('A', buf[0]);

  EXPECT_EQ(0U, cache_mgr_->Pread(fd, &buf, 1024, 1024));
  EXPECT_EQ(0U, cache_mgr_->Pread(fd, &buf, 0, 0));
  EXPECT_EQ(0U, cache_mgr_->Pread(fd, NULL, 0, 0));
  EXPECT_EQ(0, cache_mgr_->Close(fd));

  EXPECT_EQ(-EBADF, cache_mgr_->Pread(fd, &buf, 1, 0));
}


TEST_F(T_CacheManager, Rename) {
  string path_null = tmp_path_ + "/" + hash_null_.MakePath();
  string path_one = tmp_path_ + "/" + hash_one_.MakePath();

  EXPECT_EQ(0, cache_mgr_->Rename(path_null.c_str(), path_one.c_str()));
  EXPECT_FALSE(FileExists(path_null));
  EXPECT_TRUE(FileExists(path_one));
  EXPECT_EQ(0, cache_mgr_->Rename(path_one.c_str(), path_one.c_str()));
  EXPECT_TRUE(FileExists(path_one));
  EXPECT_EQ(-ENOENT, cache_mgr_->Rename(path_null.c_str(), path_one.c_str()));

  EXPECT_TRUE(CopyPath2Path(path_one, path_null));
  cache_mgr_->alien_cache_on_nfs_ = true;
  EXPECT_EQ(0, cache_mgr_->Rename(path_null.c_str(), path_one.c_str()));
  EXPECT_FALSE(FileExists(path_null));
  EXPECT_TRUE(FileExists(path_one));
  // Does not work on nfs
  //EXPECT_EQ(0, cache_mgr_->Rename(path_one.c_str(), path_one.c_str()));
  EXPECT_EQ(0, cache_mgr_->Rename(path_one.c_str(), path_null.c_str()));
  EXPECT_TRUE(FileExists(path_null));
  EXPECT_FALSE(FileExists(path_one));
  EXPECT_EQ(-ENOENT, cache_mgr_->Rename(path_one.c_str(), path_null.c_str()));
}


TEST_F(T_CacheManager, Reset) {
  char large_buf[5000];
  large_buf[0] = 'A';
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);
  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  EXPECT_EQ(5000, cache_mgr_->Write(large_buf, 5000, txn));
  EXPECT_EQ(0, cache_mgr_->Reset(txn));
  EXPECT_EQ(1, cache_mgr_->Write(large_buf, 1, txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));

  int fd = cache_mgr_->Open(rnd_hash);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(1, cache_mgr_->GetSize(fd));
  EXPECT_EQ(1, cache_mgr_->Pread(fd, large_buf, 1, 0));
  EXPECT_EQ('A', large_buf[0]);
  cache_mgr_->Close(fd);

  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);
  EXPECT_EQ(0, cache_mgr_->Reset(txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));
  fd = cache_mgr_->Open(rnd_hash);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->GetSize(fd));
  cache_mgr_->Close(fd);

  fd = cache_mgr_->StartTxn(rnd_hash, txn);
  EXPECT_EQ(0, close(fd));
  EXPECT_EQ(-EBADF, cache_mgr_->Reset(txn));
  cache_mgr_->AbortTxn(txn);
}


TEST_F(T_CacheManager, StartTxn) {
  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);
  int fd = cache_mgr_->StartTxn(rnd_hash, txn);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(0, cache_mgr_->GetSize(fd));
  cache_mgr_->AbortTxn(txn);

  EXPECT_EQ(0, rmdir((tmp_path_ + "/txn").c_str()));
  EXPECT_EQ(-ENOENT, cache_mgr_->StartTxn(rnd_hash, txn));
  MkdirDeep(tmp_path_ + "/txn", 0700);
}


TEST_F(T_CacheManager, TearDown2ReadOnly) {
  EXPECT_FALSE(TearDownTimedOut(cache_mgr_));
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  EXPECT_EQ(-EROFS, cache_mgr_->StartTxn(hash_null_, txn));

  cache_mgr_->cache_mode_ = PosixCacheManager::kCacheReadWrite;

  void *txn1 = alloca(cache_mgr_->SizeOfTxn());
  void *txn2 = alloca(cache_mgr_->SizeOfTxn());
  EXPECT_GE(cache_mgr_->StartTxn(hash_null_, txn1), 0);
  EXPECT_GE(cache_mgr_->StartTxn(hash_one_, txn2), 0);
  EXPECT_EQ(0, cache_mgr_->AbortTxn(txn1));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn2));
  EXPECT_FALSE(TearDownTimedOut(cache_mgr_));

  cache_mgr_->cache_mode_ = PosixCacheManager::kCacheReadWrite;

  EXPECT_GE(cache_mgr_->StartTxn(hash_null_, txn1), 0);
  EXPECT_GE(cache_mgr_->StartTxn(hash_one_, txn2), 0);
  pthread_t thread_teardown;
  TearDownCb cb(cache_mgr_);
  int retval = pthread_create(&thread_teardown, NULL, MainTearDown, &cb);
  assert(retval == 0);
  EXPECT_EQ(0, cache_mgr_->AbortTxn(txn1));
  SafeSleepMs(75);
  EXPECT_FALSE(cb.finished);
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn2));
  SafeSleepMs(75);
  EXPECT_TRUE(cb.finished);
  if (cb.finished)
    pthread_join(thread_teardown, NULL);
  else
    pthread_cancel(thread_teardown);
}


TEST_F(T_CacheManager, TearDown2ReadOnlyTimeout) {
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  cache_mgr_->StartTxn(hash_null_, txn);
  EXPECT_TRUE(TearDownTimedOut(cache_mgr_));
  cache_mgr_->AbortTxn(txn);
}


TEST_F(T_CacheManager, Write) {
  char large_buf[10000];
  char page_buf[4096];

  shash::Any rnd_hash;
  rnd_hash.Randomize();
  void *txn = alloca(cache_mgr_->SizeOfTxn());
  ASSERT_TRUE(txn != NULL);
  EXPECT_GE(cache_mgr_->StartTxn(rnd_hash, txn), 0);

  EXPECT_EQ(4096, cache_mgr_->Write(page_buf, 4096, txn));
  EXPECT_EQ(10000, cache_mgr_->Write(large_buf, 10000, txn));
  EXPECT_EQ(0, cache_mgr_->Write(large_buf, 0, txn));
  EXPECT_EQ(0, cache_mgr_->Write(NULL, 0, txn));
  EXPECT_EQ(0, cache_mgr_->CommitTxn(txn));

  int fd = cache_mgr_->Open(rnd_hash);
  EXPECT_GE(fd, 0);
  EXPECT_EQ(14096, cache_mgr_->GetSize(fd));
  cache_mgr_->Close(fd);

  fd = cache_mgr_->StartTxn(rnd_hash, txn);
  close(fd);
  EXPECT_EQ(-EBADF, cache_mgr_->Write(large_buf, 10000, txn));
  cache_mgr_->AbortTxn(txn);
}

}  // namespace cache